// =====================================================
// LoRa link for the StrikeSense watch (RadioLib)
//
// Receives the rod's heartbeat and strikes, confirms strikes,
// and sends the watch buttons to the rod (repeated until the
// rod confirms). Runs in a task of its own: the Bluetooth scan
// (up to 3 s) never makes it miss a packet.
// =====================================================

#include <Arduino.h>
#include <string.h>

#include "lora_link.h"

#if WATCH_LORA

#include <SPI.h>
#include <RadioLib.h>

extern "C" int watch_bat_percent(void);     // watch_bat.cpp, -1 = not measured

#if defined(ESP_PLATFORM)
#include <esp_task_wdt.h>
static portMUX_TYPE loraMux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&loraMux)
#define UNLOCK() portEXIT_CRITICAL(&loraMux)
#else
#define LOCK()
#define UNLOCK()
#endif

// ---------- Wiring: SX1262 LoRa Node (HF) -> watch header ----------

#define LORA_SCK     16   // CLK
#define LORA_MOSI    17
#define LORA_MISO    18
#define LORA_CS       5
#define LORA_RESET    6
#define LORA_BUSY     7
#define LORA_DIO1     1
#define LORA_RXEN     2
#define LORA_TXEN     3

#define CONNECTED_TIMEOUT_MS  5000

// The display already uses SPI2 (FSPI). LoRa gets its own bus: SPI3 (HSPI).
// (The SD card uses its own SD bus, see strike_sd.cpp.)
static SPIClass loraSPI(HSPI);

static SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RESET, LORA_BUSY, loraSPI,
                                 SPISettings(1000000, MSBFIRST, SPI_MODE0));   // 1 MHz: safe with hand-soldered wires

static volatile bool loraIrq = false;
static bool txBusy = false;
static uint32_t txStart = 0;

static lora_link_status_t status;

static uint8_t seq = 0;

// Command waiting for the rod's ACK (set from the UI task)
static volatile uint8_t pendingCmd = 0;
static volatile bool newCmd = false;
static volatile uint32_t pendingArg = 0;
static volatile int8_t pendingTz15 = 0;
static uint8_t cmdSeq = 0;
static int cmdTries = 0;
static uint32_t nextCmdSend = 0;

// ACK waiting to be sent
static bool ackQueued = false;
static ss_ack_t queuedAck;
static uint32_t ackDueAt = 0;
static uint16_t lastAckedStrike = 0;
static bool ackedAnyStrike = false;

#define RETRY_BEGIN_MS  5000      // the first few tries
#define RETRY_SLOW_MS   60000     // then: no module wired? try once a minute

static uint32_t lastBeginTry = 0;
static uint32_t lastWatchHb = 0;
static int beginTries = 0;

static void IRAM_ATTR onLoraIrq(void)
{
  loraIrq = true;
}

static void fillHeader(ss_header_t* h, uint8_t type)
{
  h->net = SS_NET_ID;
  h->type = type;
  h->seq = seq++;
}

static void sendRaw(const void* data, size_t len)
{
  if(radio.startTransmit((const uint8_t*)data, len) == RADIOLIB_ERR_NONE)
  {
    txBusy = true;
    txStart = millis();
  }
  else
  {
    radio.startReceive();
  }
}

static void queueAck(uint8_t ackType, uint16_t value)
{
  fillHeader(&queuedAck.h, PKT_ACK);

  queuedAck.ackType = ackType;
  queuedAck.ackValue = value;

  ackQueued = true;
  ackDueAt = millis() + 20;   // give the rod time to start listening
}

// Confirm a strike once (the rod repeats it until we do)
static void confirmStrike(uint16_t id)
{
  if(ackedAnyStrike && id == lastAckedStrike)
    return;

  queueAck(PKT_STRIKE, id);
}

static void handlePacket(const uint8_t* buf, size_t len)
{
  if(len < sizeof(ss_header_t))
    return;

  const ss_header_t* h = (const ss_header_t*)buf;

  if(h->net != SS_NET_ID)
    return;

  float rssi = radio.getRSSI();
  float snr = radio.getSNR();

  LOCK();
  status.last_rx_ms = millis();
  status.last_rssi = rssi;
  status.last_snr = snr;
  UNLOCK();

  if(h->type == PKT_HEARTBEAT && len >= SS_HEARTBEAT_V1_LEN)
  {
    ss_heartbeat_t p;
    memset(&p, 0, sizeof(p));
    memcpy(&p, buf, len < sizeof(p) ? len : sizeof(p));    // an older rod sends a shorter one
    bool ext = len >= sizeof(ss_heartbeat_t);

    LOCK();
    status.rod_state = p.state;
    status.strike_type = p.strike;
    status.strike_id = p.strikeId;
    status.sea = p.sea;
    status.rod_battery = p.battery;
    status.learn_left = p.learnLeft;
    status.tilt = p.tilt10 / 10.0f;
    status.bucket = p.bucket;
    status.knocks = p.knocks;

    status.ext = ext;
    status.ftilt = ext ? p.ftilt10 / 10.0f : status.tilt;
    status.allow = ext ? p.allow100 / 100.0f : 1.0f;
    status.drop = p.drop;
    status.flags = p.flags;
    status.knock_id = p.knockId;
    status.kpeak10 = p.kpeak10;
    status.wave = p.waveWind & 0x0F;
    status.wind = p.waveWind >> 4;
    status.bat_left = (!ext || p.batLeft == 0xFFFF) ? -1 : (long)p.batLeft;
    status.ptdeg = p.ptdeg100 / 100.0f;
    status.ptneed = ext ? p.ptneed100 / 100.0f : 1.0f;
    status.utc = p.utc;
    status.tz_min = p.tz15 * 15;

    status.last_hb_ms = millis();
    status.hb_count++;
    UNLOCK();

    // A strike we did not confirm yet (its STRIKE packet may be lost)
    if(p.strike != STRIKE_NONE)
      confirmStrike(p.strikeId);
  }
  else if(h->type == PKT_STRIKE && len >= sizeof(ss_strike_t))
  {
    const ss_strike_t* p = (const ss_strike_t*)buf;

    LOCK();
    status.strike_type = p->strike;
    status.strike_id = p->strikeId;
    status.strike_peak = p->peakTilt10 / 10.0f;
    UNLOCK();

    Serial.printf("[LoRa] STRIKE %s (id %u)\n", lora_link_strike_name(p->strike), p->strikeId);

    // Always answer: our previous ACK may have been lost
    ackedAnyStrike = false;
    confirmStrike(p->strikeId);
  }
  else if(h->type == PKT_ACK && len >= sizeof(ss_ack_t))
  {
    const ss_ack_t* a = (const ss_ack_t*)buf;

    if(a->ackType == PKT_COMMAND && a->ackValue == cmdSeq && pendingCmd != 0)
    {
      Serial.printf("[LoRa] rod confirmed command %d\n", pendingCmd);
      pendingCmd = 0;
    }
  }
}

static int16_t startRadio(float tcxoVoltage)
{
  return radio.begin(
    SS_FREQ_MHZ, SS_BW_KHZ, SS_SF, SS_CR,
    SS_SYNC_WORD, SS_POWER_DBM, SS_PREAMBLE, tcxoVoltage
  );
}

static bool startLink(void)
{
  lastBeginTry = millis();
  beginTries++;

  // The module clock is a TCXO (powered from DIO3) or a plain crystal
  int16_t state = startRadio(1.8);

  if(state != RADIOLIB_ERR_NONE)
    state = startRadio(0);

  status.init_error = state;

  if(state != RADIOLIB_ERR_NONE)
  {
    Serial.printf("[LoRa] FAILED (try %d), error %d - retrying in %d s (Bluetooth still works)\n",
                  beginTries, state, (beginTries < 3 ? RETRY_BEGIN_MS : RETRY_SLOW_MS) / 1000);

    if(state == RADIOLIB_ERR_CHIP_NOT_FOUND)
      Serial.println("[LoRa] -2 = no answer over SPI: check 3V3, GND, CLK, MOSI, MISO, CS");

    return false;
  }

  // This module has its own antenna switch on RXEN / TXEN
  radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);

  radio.setDio1Action(onLoraIrq);
  radio.startReceive();

  status.radio_ok = true;

  Serial.printf("[LoRa] OK  %.3f MHz  SF%d  %.0f kHz - waiting for the rod\n", SS_FREQ_MHZ, SS_SF, SS_BW_KHZ);

  return true;
}

// ---------- Wiring self-check (slow software SPI, no library) ----------
//
// Reads the chip name straight from the module before RadioLib starts.
// Tells us if a problem is the wires (no name) or the software (name OK).

static uint8_t bbTransfer(uint8_t out)
{
  uint8_t in = 0;

  for(int i = 7; i >= 0; i--)
  {
    digitalWrite(LORA_MOSI, (out >> i) & 1);
    delayMicroseconds(10);
    digitalWrite(LORA_SCK, HIGH);
    delayMicroseconds(10);
    in |= digitalRead(LORA_MISO) << i;
    digitalWrite(LORA_SCK, LOW);
    delayMicroseconds(10);
  }

  return in;
}

static bool wiringCheck(void)
{
  pinMode(LORA_RXEN, OUTPUT);
  digitalWrite(LORA_RXEN, LOW);
  pinMode(LORA_TXEN, OUTPUT);
  digitalWrite(LORA_TXEN, LOW);

  pinMode(LORA_BUSY, INPUT);
  pinMode(LORA_RESET, OUTPUT);
  pinMode(LORA_CS, OUTPUT);
  digitalWrite(LORA_CS, HIGH);
  pinMode(LORA_SCK, OUTPUT);
  digitalWrite(LORA_SCK, LOW);
  pinMode(LORA_MOSI, OUTPUT);
  digitalWrite(LORA_MOSI, LOW);
  pinMode(LORA_MISO, INPUT_PULLUP);

  int busyBefore = digitalRead(LORA_BUSY);

  digitalWrite(LORA_RESET, LOW);
  delay(2);
  digitalWrite(LORA_RESET, HIGH);

  uint32_t t = millis();
  while(digitalRead(LORA_BUSY) == HIGH && millis() - t < 100)
    ;
  int busyAfter = digitalRead(LORA_BUSY);

  char text[17];
  uint8_t raw[4];

  digitalWrite(LORA_CS, LOW);
  delayMicroseconds(20);
  bbTransfer(0x1D);          // ReadRegister
  bbTransfer(0x03);
  bbTransfer(0x20);          // 0x0320 = chip name
  bbTransfer(0x00);

  for(int i = 0; i < 16; i++)
  {
    uint8_t c = bbTransfer(0x00);
    if(i < 4)
      raw[i] = c;
    text[i] = (c >= 32 && c < 127) ? (char)c : '.';
  }

  text[16] = 0;
  digitalWrite(LORA_CS, HIGH);

  pinMode(LORA_MISO, INPUT);

  bool ok = strncmp(text, "SX126", 5) == 0;

  Serial.printf("[LoRa] wiring check: BUSY %d->%d  chip says \"%s\" (%02X %02X %02X %02X)  %s\n",
                busyBefore, busyAfter, text, raw[0], raw[1], raw[2], raw[3],
                ok ? "WIRING OK" : "NO ANSWER");

  if(!ok)
  {
    if(raw[0] == 0xFF && raw[1] == 0xFF)
      Serial.println("[LoRa]   all FF: MISO not connected, or the module has no power (3V3 / GND)");
    else if(raw[0] == 0x00 && raw[1] == 0x00)
      Serial.println("[LoRa]   all 00: MISO stuck low, or CLK / CS / MOSI not reaching the module");
    else
      Serial.println("[LoRa]   random bytes: a loose or mixed-up SPI wire (CLK, MOSI, MISO, CS)");

    if(busyAfter == HIGH)
      Serial.println("[LoRa]   BUSY stays HIGH: check RESET (IO6), BUSY (IO7) and the 3V3 supply");
  }

  return ok;
}

bool lora_link_begin(void)
{
  memset(&status, 0, sizeof(status));
  status.rod_battery = 255;

  // Random start, so a restarted watch never repeats the rod's last seen number
  seq = (uint8_t)random(0, 256);

  Serial.println();
  Serial.println("[LoRa] starting SX1262...");

  wiringCheck();

  loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

  return startLink();
}

void lora_link_poll(void)
{
  // Radio did not start: try again every few seconds
  if(!status.radio_ok)
  {
    if(millis() - lastBeginTry >= (uint32_t)(beginTries < 3 ? RETRY_BEGIN_MS : RETRY_SLOW_MS))
      startLink();

    return;
  }

  uint32_t now = millis();

  // ---------- Radio finished sending or received something ----------

  if(loraIrq)
  {
    loraIrq = false;

    if(txBusy)
    {
      radio.finishTransmit();
      txBusy = false;
    }
    else
    {
      uint8_t buf[64];
      size_t len = radio.getPacketLength();

      if(len > 0 && len <= sizeof(buf) && radio.readData(buf, len) == RADIOLIB_ERR_NONE)
        handlePacket(buf, len);
    }

    radio.startReceive();
  }

  if(txBusy && now - txStart > 2000)
  {
    txBusy = false;
    radio.startReceive();
  }

  LOCK();
  status.connected =
    status.last_hb_ms != 0 &&
    now - status.last_hb_ms < CONNECTED_TIMEOUT_MS;
  UNLOCK();

  if(txBusy)
    return;

  // ---------- What to send next ----------

  if(ackQueued && (int32_t)(now - ackDueAt) >= 0)
  {
    ackQueued = false;

    if(queuedAck.ackType == PKT_STRIKE)
    {
      lastAckedStrike = queuedAck.ackValue;
      ackedAnyStrike = true;
    }

    sendRaw(&queuedAck, sizeof(queuedAck));
    return;
  }

  if(newCmd)
  {
    newCmd = false;
    cmdSeq = seq;        // the seq the command packet is about to get
    cmdTries = 0;
    nextCmdSend = now;
  }

  if(pendingCmd != 0 && (int32_t)(now - nextCmdSend) >= 0)
  {
    ss_command_t c;
    fillHeader(&c.h, PKT_COMMAND);
    c.h.seq = cmdSeq;    // repeats keep the same number
    c.cmd = pendingCmd;
    c.arg = c.cmd == CMD_SETTIME ? pendingArg : 0;
    c.tz15 = c.cmd == CMD_SETTIME ? pendingTz15 : 0;

    sendRaw(&c, sizeof(c));

    cmdTries++;
    nextCmdSend = now + SS_RESEND_MS + random(0, 300);

    if(cmdTries >= SS_MAX_TRIES)
    {
      Serial.printf("[LoRa] rod did not confirm command %d\n", pendingCmd);
      pendingCmd = 0;
    }

    return;
  }

  // "I'm here" for the rod's dashboard (only once we hear the rod)
  if(status.connected && now - lastWatchHb >= SS_WATCH_HB_MS + (uint32_t)random(0, 500))
  {
    lastWatchHb = now;

    ss_watch_hb_t w;
    fillHeader(&w.h, PKT_WATCH_HB);
    w.rodRssi = (int8_t)constrain((int)status.last_rssi, -127, 0);
    w.rodSnr = (int8_t)constrain((int)status.last_snr, -30, 30);
    int bat = watch_bat_percent();
    w.battery = bat < 0 ? 255 : (uint8_t)bat;

    sendRaw(&w, sizeof(w));
  }
}

lora_link_status_t lora_link_get_status(void)
{
  lora_link_status_t s;

  LOCK();
  s = status;
  UNLOCK();

  return s;
}

#if defined(ESP_PLATFORM)
static void loraTask(void *)
{
  esp_task_wdt_add(NULL);           // a stuck radio restarts the watch (30 s)
  lora_link_begin();

  for(;;)
  {
    esp_task_wdt_reset();
    lora_link_poll();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void lora_link_start_task(void)
{
  xTaskCreatePinnedToCore(loraTask, "lora", 4096, NULL, 1, NULL, 1);
}
#else
void lora_link_start_task(void)
{
  lora_link_begin();
}
#endif

uint8_t lora_link_signal_bars(void)
{
  if(status.last_rx_ms == 0 || millis() - status.last_rx_ms > 6000)
    return 0;

  if(status.last_rssi > -80)  return 4;
  if(status.last_rssi > -95)  return 3;
  if(status.last_rssi > -108) return 2;

  return 1;
}

void lora_link_send_command(uint8_t cmd)
{
  pendingCmd = cmd;
  newCmd = true;
}

void lora_link_send_settime(uint32_t utc, int tz_min)
{
  pendingArg = utc;
  pendingTz15 = (int8_t)(tz_min / 15);
  lora_link_send_command(CMD_SETTIME);
}

bool lora_link_command_pending(void)
{
  return pendingCmd != 0;
}

#else   // WATCH_LORA 0: Bluetooth only

bool lora_link_begin(void) { return false; }
void lora_link_poll(void) {}
void lora_link_start_task(void) {}
lora_link_status_t lora_link_get_status(void) { lora_link_status_t s; memset(&s, 0, sizeof(s)); return s; }
uint8_t lora_link_signal_bars(void) { return 0; }
void lora_link_send_command(uint8_t) {}
void lora_link_send_settime(uint32_t, int) {}
bool lora_link_command_pending(void) { return false; }

#endif

const char* lora_link_strike_name(uint8_t strike_type)
{
  switch(strike_type)
  {
    case STRIKE_SHARP:    return "SHARP STRIKE";
    case STRIKE_SLOW:     return "SLOW PULL";
    case STRIKE_REPEATED: return "REPEATED BITE";
    case STRIKE_DROPBACK: return "DROP-BACK + PULL";
    case STRIKE_SHAKE:    return "SHAKING FISH";
    default:              return "SIM STRIKE";
  }
}

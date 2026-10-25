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
#include <Preferences.h>
#include <math.h>
#include <stddef.h>
#include "ss_crypto.h"

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

// ---------- private LoRa: our rod's key, message numbers ----------

static ss_aes_t aes;
static bool keyed = false;
static uint16_t rodTag = 0;
static uint32_t txCtr = 0;               // never repeats: boot count << 20 | n (bit 31: from the watch)
static uint32_t lastRodCtr = 0;          // a copied (replayed) rod packet is ignored
static volatile bool newKey = false;     // set from the Bluetooth task
static uint8_t keyIn[16];
static uint16_t tagIn = 0;

// ---------- channels: follow the rod; search when it is lost ----------

static uint8_t curChan = CH_FAST;
static bool searching = false;
static uint32_t searchAt = 0;
static uint32_t rodGapMs = 1000;         // the rod's calm heartbeat gap
static uint32_t backoffUntil = 0;

// missed heartbeats (the rod moves to long range when we miss many)
static uint32_t winStart = 0;
static int winGot = 0;
static uint8_t lossPct = 0;

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

// encrypted + signed with our rod's key, after listening (false = try again soon)
static bool sendRaw(const void* data, size_t len, bool urgent = false)
{
  uint8_t out[96];

  if(!keyed || len + SS_WIRE_OVERHEAD > sizeof(out))
    return false;

  if(!urgent)
  {
    int16_t cad = radio.scanChannel();
    loraIrq = false;

    if(cad == RADIOLIB_LORA_DETECTED)
    {
      backoffUntil = millis() + random(20, 80);
      radio.startReceive();
      return false;
    }
  }

  ss_wire_t w = { SS_MAGIC, 1, rodTag, txCtr++ };
  memcpy(out, &w, sizeof(w));
  memcpy(out + sizeof(w), data, len);

  uint8_t nonce[16] = { 0 };
  memcpy(nonce, &w, sizeof(w));
  ss_aes_ctr(&aes, nonce, out + sizeof(w), len);

  uint8_t mac[16];
  ss_aes_cmac(&aes, out, sizeof(w) + len, mac);
  memcpy(out + sizeof(w) + len, mac, SS_MAC_LEN);

  if(radio.startTransmit(out, len + SS_WIRE_OVERHEAD) == RADIOLIB_ERR_NONE)
  {
    txBusy = true;
    txStart = millis();
    return true;
  }

  radio.startReceive();
  return false;
}

static void setChannel(uint8_t ch)
{
  if(ch >= CH_COUNT)
    return;

  const ss_channel_t &c = SS_CHANNELS[ch];

  radio.standby();
  radio.setFrequency(c.mhz);
  radio.setBandwidth(c.khz);
  radio.setSpreadingFactor(c.sf);
  radio.setOutputPower(c.dbm);
  radio.startReceive();

  curChan = ch;

  LOCK();
  status.chan = ch;
  UNLOCK();
}

static void loadKey(void)
{
  Preferences p;
  p.begin("pair", false);
  String k = p.getString("lkey", "");
  rodTag = (uint16_t)p.getULong("ltag", 0);
  uint32_t boot = p.getULong("lboot", 0) + 1;
  p.putULong("lboot", boot);
  p.end();

  txCtr = 0x80000000u | ((boot & 0x7FF) << 20);
  keyed = false;

  if(k.length() == 32)
  {
    uint8_t key[16];

    for(int i = 0; i < 16; i++)
      key[i] = (uint8_t)strtol(k.substring(2 * i, 2 * i + 2).c_str(), nullptr, 16);

    ss_aes_init(&aes, key);
    keyed = true;
  }

  status.keyed = keyed;
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

static void handlePacket(const uint8_t* buf, size_t len);

// anything on the air: only our own rod's packets are opened
static void handleRaw(const uint8_t* buf, size_t len)
{
  if(!keyed || len < SS_WIRE_OVERHEAD + sizeof(ss_header_t) || buf[0] != SS_MAGIC)
    return;

  ss_wire_t w;
  memcpy(&w, buf, sizeof(w));
  size_t n = len - SS_WIRE_OVERHEAD;

  if(w.rod != rodTag || w.from != 0 || n > 64)
    return;                                     // another rod, or a watch

  uint8_t mac[16];
  ss_aes_cmac(&aes, buf, sizeof(w) + n, mac);

  if(memcmp(mac, buf + sizeof(w) + n, SS_MAC_LEN) != 0)
    return;                                     // not our rod's key, or changed on the way

  if(lastRodCtr != 0 && w.ctr <= lastRodCtr && (lastRodCtr - w.ctr) < 0x100000u)
    return;                                     // a copy of an old packet

  lastRodCtr = w.ctr;

  uint8_t plain[64];
  memcpy(plain, buf + sizeof(w), n);

  uint8_t nonce[16] = { 0 };
  memcpy(nonce, &w, sizeof(w));
  ss_aes_ctr(&aes, nonce, plain, n);

  handlePacket(plain, n);
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
    bool ext = len >= offsetof(ss_heartbeat_t, chan);       // rod 2.3 fields
    bool v24 = len >= sizeof(ss_heartbeat_t);                // rod 2.4: channel, crowd

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

    if(v24)
    {
      status.lmode = p.modes & 3;
      status.chm = p.modes >> 2;
      status.quality = p.quality;
      status.rods = p.rods;
      status.crowd = (p.flags & SS_F_CROWD) != 0;
    }

    status.last_hb_ms = millis();
    status.hb_count++;
    status.searching = false;
    UNLOCK();

    if(searching)
    {
      winStart = millis();         // count missed heartbeats only from now on
      winGot = 0;
    }

    searching = false;
    winGot++;

    if(v24 && p.gap10 > 0)
      rodGapMs = p.gap10 * 100UL;

    if(v24 && p.chan != curChan && p.chan < CH_COUNT)
      setChannel(p.chan);

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
  else if(h->type == PKT_MOVE && len >= sizeof(ss_move_t))
  {
    const ss_move_t* m = (const ss_move_t*)buf;
    Serial.printf("[LoRa] the rod moves to channel %s\n", lora_link_chan_name(m->chan));
    setChannel(m->chan);
    winStart = millis();
    winGot = 0;
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
  const ss_channel_t &c = SS_CHANNELS[CH_FAST];
  return radio.begin(c.mhz, c.khz, c.sf, SS_CR, SS_SYNC_WORD, c.dbm, SS_PREAMBLE, tcxoVoltage);
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

  curChan = CH_FAST;
  status.chan = CH_FAST;

  if(keyed)
    Serial.printf("[LoRa] OK - waiting for our rod (tag %04X), encrypted\n", rodTag);
  else
    Serial.println("[LoRa] OK - no rod key yet: pair the watch with its rod over Bluetooth first");

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
  status.lmode = -1;
  status.chm = -1;

  // Random start, so a restarted watch never repeats the rod's last seen number
  seq = (uint8_t)random(0, 256);

  loadKey();

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
      uint8_t buf[96];
      size_t len = radio.getPacketLength();

      if(len > 0 && len <= sizeof(buf) && radio.readData(buf, len) == RADIOLIB_ERR_NONE)
        handleRaw(buf, len);
    }

    radio.startReceive();
  }

  if(txBusy && now - txStart > 2000)
  {
    txBusy = false;
    radio.startReceive();
  }

  // a new key from the Bluetooth task (just paired / rod changed)
  if(newKey)
  {
    newKey = false;
    char hex[33];

    for(int i = 0; i < 16; i++)
      snprintf(hex + 2 * i, 3, "%02x", keyIn[i]);

    Preferences p;
    p.begin("pair", false);
    p.putString("lkey", String(hex));
    p.putULong("ltag", tagIn);
    p.end();

    rodTag = tagIn;
    ss_aes_init(&aes, keyIn);
    keyed = true;
    lastRodCtr = 0;

    LOCK();
    status.keyed = true;
    UNLOCK();

    Serial.printf("[LoRa] got our rod's key (tag %04X)\n", rodTag);
  }

  uint32_t silent = status.last_hb_ms == 0 ? now : now - status.last_hb_ms;
  uint32_t lostAfter = rodGapMs * 3 + 1500 > 6000 ? rodGapMs * 3 + 1500 : 6000;

  LOCK();
  status.connected =
    status.last_hb_ms != 0 &&
    silent < (CONNECTED_TIMEOUT_MS > lostAfter ? CONNECTED_TIMEOUT_MS : lostAfter);
  UNLOCK();

  // missed heartbeats over ~15 s (sent to the rod: it changes channel if many)
  if(winStart == 0)
    winStart = now;

  if(now - winStart >= 15000)
  {
    float expected = (now - winStart) / (float)rodGapMs;
    float miss = expected > 0 ? 1.0f - winGot / expected : 0;
    lossPct = (uint8_t)(miss < 0 ? 0 : miss > 1 ? 100 : miss * 100);
    winStart = now;
    winGot = 0;

    LOCK();
    status.loss = lossPct;
    UNLOCK();
  }

  // rod not heard: try each channel in turn (it may have moved without us)
  if(keyed && !txBusy && (status.last_hb_ms == 0 || silent > lostAfter))
  {
    uint32_t dwell = rodGapMs * 2 + 500 > 4000 ? rodGapMs * 2 + 500 : 4000;

    if(!searching)
    {
      // first wait here a while (the rod may just be quiet), then try the others
      searching = true;
      searchAt = now;

      LOCK();
      status.searching = true;
      UNLOCK();
    }
    else if(now - searchAt >= dwell)
    {
      searchAt = now;
      setChannel((uint8_t)((curChan + 1) % CH_COUNT));
    }
  }

  if(txBusy)
    return;

  if((int32_t)(now - backoffUntil) < 0)
    return;                        // someone else is talking: wait our turn

  // ---------- What to send next ----------

  if(ackQueued && (int32_t)(now - ackDueAt) >= 0)
  {
    ackQueued = false;

    if(queuedAck.ackType == PKT_STRIKE)
    {
      lastAckedStrike = queuedAck.ackValue;
      ackedAnyStrike = true;
    }

    sendRaw(&queuedAck, sizeof(queuedAck), true);
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
    c.arg = pendingArg;
    c.tz15 = c.cmd == CMD_SETTIME ? pendingTz15 : 0;

    if(!sendRaw(&c, sizeof(c)))
      return;

    cmdTries++;
    nextCmdSend = now + SS_RESEND_MS + random(0, 300);

    if(cmdTries >= SS_MAX_TRIES)
    {
      Serial.printf("[LoRa] rod did not confirm command %d\n", pendingCmd);
      pendingCmd = 0;
    }

    return;
  }

  // "I'm here, hearing you this well" (only once we hear the rod; less often in a crowd)
  uint32_t hbEvery = status.crowd ? 15000 : SS_WATCH_HB_MS;

  if(status.connected && !searching && now - lastWatchHb >= hbEvery + (uint32_t)random(0, 500))
  {
    ss_watch_hb_t w;
    fillHeader(&w.h, PKT_WATCH_HB);
    w.rodRssi = (int8_t)constrain((int)status.last_rssi, -127, 0);
    w.rodSnr = (int8_t)constrain((int)status.last_snr, -30, 30);
    int bat = watch_bat_percent();
    w.battery = bat < 0 ? 255 : (uint8_t)bat;
    w.loss = lossPct;

    if(sendRaw(&w, sizeof(w)))
      lastWatchHb = now;
  }
}

void lora_link_set_key(uint16_t rod_tag, const uint8_t key[16])
{
  memcpy(keyIn, key, 16);
  tagIn = rod_tag;
  newKey = true;
}

bool lora_link_has_key(uint16_t rod_tag)
{
  return keyed && rodTag == rod_tag;
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
  pendingArg = 0;
  pendingCmd = cmd;
  newCmd = true;
}

void lora_link_send_command_arg(uint8_t cmd, uint32_t arg)
{
  pendingArg = arg;
  pendingTz15 = 0;
  pendingCmd = cmd;
  newCmd = true;
}

void lora_link_send_settime(uint32_t utc, int tz_min)
{
  pendingArg = utc;
  pendingTz15 = (int8_t)(tz_min / 15);
  pendingCmd = CMD_SETTIME;
  newCmd = true;
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
void lora_link_send_command_arg(uint8_t, uint32_t) {}
bool lora_link_command_pending(void) { return false; }
void lora_link_set_key(uint16_t, const uint8_t *) {}
bool lora_link_has_key(uint16_t) { return true; }

#endif

const char* lora_link_chan_name(uint8_t chan)
{
  return chan == CH_FAST ? "500 m" : "1 km";
}

const char* lora_link_quality_name(uint8_t q)
{
  switch(q)
  {
    case Q_CLEAR: return "Clear";
    case Q_GOOD:  return "Good";
    case Q_BUSY:  return "Busy";
    default:      return "Congested";
  }
}

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

// =====================================================
// LoRa link for the StrikeSense watch (RadioLib)
//
// Receives the rod's heartbeat and strikes, confirms strikes,
// and sends the watch buttons to the rod (repeated until the
// rod confirms). Never blocks: everything runs from loop().
// =====================================================

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

#include "lora_link.h"

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
static uint8_t cmdSeq = 0;
static int cmdTries = 0;
static uint32_t nextCmdSend = 0;

// ACK waiting to be sent
static bool ackQueued = false;
static ss_ack_t queuedAck;
static uint32_t ackDueAt = 0;
static uint16_t lastAckedStrike = 0;
static bool ackedAnyStrike = false;

#define RETRY_BEGIN_MS  5000

static uint32_t lastBeginTry = 0;
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

  status.last_rx_ms = millis();
  status.last_rssi = radio.getRSSI();
  status.last_snr = radio.getSNR();

  if(h->type == PKT_HEARTBEAT && len >= sizeof(ss_heartbeat_t))
  {
    const ss_heartbeat_t* p = (const ss_heartbeat_t*)buf;

    status.rod_state = p->state;
    status.strike_type = p->strike;
    status.strike_id = p->strikeId;
    status.sea = p->sea;
    status.rod_battery = p->battery;
    status.learn_left = p->learnLeft;
    status.tilt = p->tilt10 / 10.0f;
    status.bucket = p->bucket;
    status.knocks = p->knocks;

    status.last_hb_ms = millis();
    status.hb_count++;

    // A strike we did not confirm yet (its STRIKE packet may be lost)
    if(p->strike != STRIKE_NONE)
      confirmStrike(p->strikeId);
  }
  else if(h->type == PKT_STRIKE && len >= sizeof(ss_strike_t))
  {
    const ss_strike_t* p = (const ss_strike_t*)buf;

    status.strike_type = p->strike;
    status.strike_id = p->strikeId;
    status.strike_peak = p->peakTilt10 / 10.0f;

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
    Serial.printf("[LoRa] FAILED (try %d), error %d - retrying in 5 s\n", beginTries, state);

    if(state == RADIOLIB_ERR_CHIP_NOT_FOUND)
      Serial.println("[LoRa] -2 = no answer over SPI: check 3V3, GND, CLK, MOSI, MISO, CS");

    return false;
  }

  // This module has its own antenna switch on RXEN / TXEN
  radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);

  radio.setDio1Action(onLoraIrq);
  radio.startReceive();

  status.radio_ok = true;

  Serial.printf("[LoRa] OK  %.3f MHz  SF%d - waiting for the rod\n", SS_FREQ_MHZ, SS_SF);

  return true;
}

bool lora_link_begin(void)
{
  memset(&status, 0, sizeof(status));
  status.rod_battery = 255;

  // Random start, so a restarted watch never repeats the rod's last seen number
  seq = (uint8_t)random(0, 256);

  loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

  Serial.println();
  Serial.println("[LoRa] starting SX1262...");

  return startLink();
}

void lora_link_poll(void)
{
  // Radio did not start: try again every few seconds
  if(!status.radio_ok)
  {
    if(millis() - lastBeginTry >= RETRY_BEGIN_MS)
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

  status.connected =
    status.last_hb_ms != 0 &&
    now - status.last_hb_ms < CONNECTED_TIMEOUT_MS;

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

    sendRaw(&c, sizeof(c));

    cmdTries++;
    nextCmdSend = now + SS_RESEND_MS + random(0, 300);

    if(cmdTries >= SS_MAX_TRIES)
    {
      Serial.printf("[LoRa] rod did not confirm command %d\n", pendingCmd);
      pendingCmd = 0;
    }
  }
}

lora_link_status_t lora_link_get_status(void)
{
  return status;
}

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

bool lora_link_command_pending(void)
{
  return pendingCmd != 0;
}

const char* lora_link_strike_name(uint8_t strike_type)
{
  switch(strike_type)
  {
    case STRIKE_SHARP:    return "SHARP STRIKE";
    case STRIKE_SLOW:     return "SLOW PULL";
    case STRIKE_REPEATED: return "REPEATED BITE";
    case STRIKE_DROPBACK: return "DROP-BACK";
    default:              return "SIM STRIKE";
  }
}

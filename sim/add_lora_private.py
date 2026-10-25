# Rod 2.4 LoRa (StrikeSense.ino only):
# - private: every packet encrypted + signed with the rod's own key (given
#   to its watch over the encrypted Bluetooth pairing: /lorakey); other
#   rods' packets are only counted
# - auto channel: fast (~500 m, 2-4 news a second) first, long range
#   (~1 km) when the watch hears the rod badly or not at all, back to fast
#   when it is strong again for 1 min; or a fixed channel (/lorachan)
# - listen before talk, the 10% transmit time rule on the long channels
# - crowd mode: many rods heard -> calm news less often; channel quality
#   Clear / Good / Busy / Congested for the watch and the app
# - strikes repeated every 0.3 s for the first 2 s until confirmed
#
#   python3 sim/add_lora_private.py

import sys

PATH = "StrikeSense/StrikeSense.ino"

NEW = r'''bool loraOk = false;
volatile bool loraIrq = false;
bool loraTxBusy = false;
unsigned long loraTxStart = 0;

uint8_t loraSeq = 0;
unsigned long lastHeartbeatSent = 0;
unsigned long lastKnockIdSent = 0;     // a new knock makes the next heartbeats fast

// Strike waiting for the watch to confirm
bool strikeNeedsAck = false;
uint16_t strikeAckId = 0;
int strikeTries = 0;
unsigned long nextStrikeSend = 0;
unsigned long strikeFirstSent = 0;

// ACK waiting to be sent (short delay so the other side is listening)
bool ackQueued = false;
ss_ack_t queuedAck;
unsigned long ackDueAt = 0;

int lastCommandSeq = -1;

// What the rod knows about the watch (any packet from it counts)
unsigned long watchLastSeen = 0;
float watchRssi = 0;            // how strong the rod hears the watch
float watchSnr = 0;
int watchHearsRodRssi = 0;      // reported by the watch
int watchHearsRodSnr = 0;
int watchLoss = 0;              // % of our heartbeats the watch missed
unsigned long watchReportAt = 0;
bool watchReported = false;

// ---------- private LoRa: the rod's key, tag, message numbers ----------

ss_aes_t loraAes;
uint8_t loraKey[16];
uint16_t rodTag = 0;            // ROD-4F2A -> 0x4F2A
uint32_t txCtr = 0;             // never repeats: boot count << 20 | n
uint32_t lastWatchCtr = 0;      // a copied (replayed) watch packet is ignored

// ---------- channels ----------

int curChan = CH_FAST;
int chanMode = CHM_AUTO;
unsigned long chanSince = 0;
unsigned long strongSince = 0;
bool moveQueued = false;        // tell the watch, then change channel
bool moveAfterTx = false;
int moveTo = CH_FAST;
unsigned long loraBackoffUntil = 0;
float dutyTokens = 20000;       // ms of airtime still allowed on a 10% channel
unsigned long dutyAt = 0;

// ---------- other rods (crowd mode) ----------

struct OtherRod
{
  uint16_t tag;
  unsigned long at;
};

const int OTHERS_MAX = 40;
OtherRod others[OTHERS_MAX];
float otherAirMs = 0;           // their airtime, last ~10 s
unsigned long otherAirAt = 0;
bool crowd = false;

void IRAM_ATTR onLoraIrq()
{
  loraIrq = true;
}

uint8_t strikeCode(const String& type)
{
  if(type == "SHARP STRIKE") return STRIKE_SHARP;
  if(type == "SLOW PULL") return STRIKE_SLOW;
  if(type == "REPEATED BITE") return STRIKE_REPEATED;
  if(type.startsWith("DROP")) return STRIKE_DROPBACK;   // DROP-BACK + PULL
  if(type == "SHAKING FISH") return STRIKE_SHAKE;

  return STRIKE_NONE;
}

uint8_t rodStateCode()
{
  switch(rodState)
  {
    case ST_LEARNING: return ROD_LEARNING;
    case ST_PULLTEST: return ROD_PULLTEST;
    case ST_SETTLING: return ROD_SETTLING;
    case ST_PAUSED: return ROD_PAUSED;
    default: return ROD_MONITORING;
  }
}

// SX1262 time on air (explicit header, CRC, coding rate 4/5), ms
float loraAirtimeMs(size_t bytes)
{
  const ss_channel_t& c = SS_CHANNELS[curChan];
  float ts = (float)(1 << c.sf) / c.khz;
  float n = 8.0f * bytes - 4 * c.sf + 28 + 16;
  int payload = 8 + (int)(ceilf(n / (4.0f * c.sf)) * 5);
  return (SS_PREAMBLE + 4.25f + payload) * ts;
}

// The rod's key (made once, kept in flash) and message numbers
void loraKeyBegin()
{
  Preferences p;
  p.begin("strikesense", false);
  String k = p.getString("lkey", "");

  if(k.length() != 32)
  {
    char hex[33];

    for(int i = 0; i < 16; i++)
    {
      loraKey[i] = (uint8_t)(esp_random() & 0xFF);
      snprintf(hex + 2 * i, 3, "%02x", loraKey[i]);
    }

    p.putString("lkey", String(hex));
  }
  else
  {
    for(int i = 0; i < 16; i++)
      loraKey[i] = (uint8_t)strtol(k.substring(2 * i, 2 * i + 2).c_str(), nullptr, 16);
  }

  uint32_t boot = p.getULong("lboot", 0) + 1;
  p.putULong("lboot", boot);
  chanMode = p.getInt("lchm", CHM_AUTO);
  p.end();

  if(chanMode < CHM_AUTO || chanMode > CHM_LONG2)
    chanMode = CHM_AUTO;

  txCtr = (boot & 0xFFF) << 20;
  rodTag = (uint16_t)strtol(rodId.c_str() + 4, nullptr, 16);
  ss_aes_init(&loraAes, loraKey);
}

// ---------- other rods: how busy is the channel ----------

void noteOtherRod(uint16_t tag, float airMs)
{
  unsigned long now = millis();
  int free = -1, oldest = 0;

  otherAirMs += airMs;

  for(int i = 0; i < OTHERS_MAX; i++)
  {
    if(others[i].at != 0 && others[i].tag == tag)
    {
      others[i].at = now;
      return;
    }

    if(others[i].at == 0 && free < 0)
      free = i;

    if(others[i].at < others[oldest].at)
      oldest = i;
  }

  others[free >= 0 ? free : oldest] = { tag, now };
}

int othersCount()
{
  int n = 0;
  unsigned long now = millis();

  for(int i = 0; i < OTHERS_MAX; i++)
    if(others[i].at != 0 && now - others[i].at < 30000)
      n++;

  return n;
}

void forgetOthers()
{
  memset(others, 0, sizeof(others));
  otherAirMs = 0;
  crowd = false;
}

uint8_t loraQuality()
{
  int n = othersCount();
  float load = otherAirMs / 10000.0f;

  if(crowd) return Q_CONGESTED;
  if(n >= 4 || load >= 0.15f) return Q_BUSY;
  if(n >= 1 || load >= 0.05f) return Q_GOOD;

  return Q_CLEAR;
}

// calm news: every second, slower when the channel is crowded
unsigned long calmGapMs()
{
  if(crowd)
    return constrain(500UL * (othersCount() + 1), 2000UL, 8000UL);

  return SS_CHANNELS[curChan].limited ? 1500 : 1000;
}

// something happening: fast channel 4 a second (2 in a crowd), long range
// about 1 a second (a heartbeat takes ~0.1 s there: 10% transmit time rule)
unsigned long busyGapMs()
{
  if(curChan == CH_FAST)
    return crowd ? 500 : 250;

  return 1100;
}

// ---------- sending: encrypted, after listening ----------

void fillHeader(ss_header_t& h, uint8_t type)
{
  h.net = SS_NET_ID;
  h.type = type;
  h.seq = loraSeq++;
}

// false = not sent now (channel busy, or the 10% rule): try again soon
bool loraSend(const void* data, size_t len, bool urgent = false)
{
  uint8_t out[96];
  const ss_channel_t& c = SS_CHANNELS[curChan];

  if(len + SS_WIRE_OVERHEAD > sizeof(out))
    return false;

  float air = loraAirtimeMs(len + SS_WIRE_OVERHEAD);

  // 10% transmit time on the long range channels (strikes may use the reserve)
  if(c.limited && dutyTokens < air + (urgent ? 0 : 2000))
    return false;

  // listen before talk: another LoRa packet on the air? wait a moment
  int16_t cad = radio.scanChannel();
  loraIrq = false;

  if(cad == RADIOLIB_LORA_DETECTED)
  {
    loraBackoffUntil = millis() + (urgent ? random(5, 25) : random(20, 80));
    radio.startReceive();
    return false;
  }

  ss_wire_t w = { SS_MAGIC, 0, rodTag, txCtr++ };
  memcpy(out, &w, sizeof(w));
  memcpy(out + sizeof(w), data, len);

  uint8_t nonce[16] = { 0 };
  memcpy(nonce, &w, sizeof(w));
  ss_aes_ctr(&loraAes, nonce, out + sizeof(w), len);

  uint8_t mac[16];
  ss_aes_cmac(&loraAes, out, sizeof(w) + len, mac);
  memcpy(out + sizeof(w) + len, mac, SS_MAC_LEN);

  if(radio.startTransmit(out, len + SS_WIRE_OVERHEAD) != RADIOLIB_ERR_NONE)
  {
    radio.startReceive();
    return false;
  }

  loraTxBusy = true;
  loraTxStart = millis();

  if(c.limited)
    dutyTokens -= air;

  return true;
}

bool sendHeartbeat()
{
  ss_heartbeat_t p;
  memset(&p, 0, sizeof(p));
  fillHeader(p.h, PKT_HEARTBEAT);

  unsigned long learnLeft = 0;

  if(rodState == ST_LEARNING && millis() - learningStart < LEARN_TIME)
    learnLeft = (LEARN_TIME - (millis() - learningStart) + 999) / 1000;

  p.state = rodStateCode();
  p.strike = strikeActive ? strikeCode(strikeType) : (uint8_t)STRIKE_NONE;
  p.strikeId = (uint16_t)strikeEventID;
  p.sea = seaState;
  p.battery = batPercent < 0 ? 255 : (uint8_t)clampFloat(batPercent, 0, 100);
  p.learnLeft = (uint8_t)learnLeft;
  p.tilt10 = (int16_t)clampFloat(tiltDeg * 10.0, -32000, 32000);
  p.bucket = (uint8_t)clampFloat(pullBucket / bucketSize * 100.0, 0, 100);
  p.knocks = (uint8_t)recentKnockCount();

  // rod 2.3+: the watch screens without Bluetooth
  p.ftilt10 = (int16_t)clampFloat(fastTiltDeg * 10.0, -32000, 32000);
  p.allow100 = (uint16_t)clampFloat(pullAllow * 100.0, 0, 65000);
  p.drop = (uint8_t)clampFloat(dropBucket / bucketSize * 100.0, 0, 100);
  p.flags = (dropWatch ? SS_F_DROPWATCH : 0) | (haveDir ? SS_F_DIR : 0) | (linkMode == 2 ? SS_F_LORA_ONLY : 0) | (crowd ? SS_F_CROWD : 0);
  p.knockId = (uint8_t)knockAlertID;
  p.kpeak10 = (uint8_t)clampFloat(knockThreshold > 0 ? energyPeakSinceLora / knockThreshold * 10.0 : 0, 0, 255);
  p.waveWind = (uint8_t)((waveLevel & 0x0F) | ((windLevel & 0x0F) << 4));
  p.batLeft = batLeftMin < 0 ? 0xFFFF : (uint16_t)clampFloat(batLeftMin, 0, 65000);
  p.ptdeg100 = (uint16_t)clampFloat(totalDeg * 100.0, 0, 65000);
  p.ptneed100 = (uint16_t)clampFloat(pullTestNeed() * 100.0, 0, 65000);
  p.utc = (uint32_t)nowEpoch();
  p.tz15 = (int8_t)(clockTzMin / 15);

  // rod 2.4: channel, quality, crowd
  p.chan = (uint8_t)curChan;
  p.quality = loraQuality();
  p.rods = (uint8_t)min(othersCount(), 255);
  p.gap10 = (uint8_t)min(calmGapMs() / 100, 255UL);

  if(!loraSend(&p, sizeof(p)))
    return false;

  energyPeakSinceLora = 0;
  lastKnockIdSent = knockAlertID;
  return true;
}

// Something happening: the watch gets news faster
bool loraBusy()
{
  return strikeActive || rodState == ST_PULLTEST ||
         pullBucket / bucketSize >= 0.10 || dropBucket / bucketSize >= 0.10 ||
         dropWatch || knockAlertID != lastKnockIdSent || recentKnockCount() > 0;
}

bool sendStrike()
{
  ss_strike_t p;
  fillHeader(p.h, PKT_STRIKE);

  p.strike = strikeCode(strikeType);
  p.strikeId = strikeAckId;
  p.peakTilt10 = (int16_t)clampFloat(strikePeakTilt * 10.0, -32000, 32000);

  return loraSend(&p, sizeof(p), true);
}

void queueAck(uint8_t ackType, uint16_t value)
{
  fillHeader(queuedAck.h, PKT_ACK);

  queuedAck.ackType = ackType;
  queuedAck.ackValue = value;

  ackQueued = true;
  ackDueAt = millis() + 20;
}

// Called by startStrike()
void loraOnStrike()
{
  if(!loraOk)
    return;

  strikeNeedsAck = true;
  strikeAckId = (uint16_t)strikeEventID;
  strikeTries = 0;
  strikeFirstSent = millis();
  nextStrikeSend = millis();
}

// ---------- channels ----------

void applyChannel(int ch)
{
  const ss_channel_t& c = SS_CHANNELS[ch];

  radio.standby();
  radio.setFrequency(c.mhz);
  radio.setBandwidth(c.khz);
  radio.setSpreadingFactor(c.sf);
  radio.setOutputPower(c.dbm);
  radio.startReceive();

  curChan = ch;
  chanSince = millis();
  strongSince = 0;
  forgetOthers();                  // other rods there are counted again

  Serial.printf("[LoRa] channel: %s (%.4f MHz)\n", ch == CH_FAST ? "FAST ~500 m" : ch == CH_LONG1 ? "LONG RANGE 1" : "LONG RANGE 2", c.mhz);
}

// the quieter long range channel (a quick listen on each)
int quieterLong()
{
  int busy[2] = { 0, 0 };

  for(int k = 0; k < 2; k++)
  {
    const ss_channel_t& c = SS_CHANNELS[CH_LONG1 + k];
    radio.standby();
    radio.setFrequency(c.mhz);
    radio.setBandwidth(c.khz);

    for(int i = 0; i < 4; i++)
      if(radio.scanChannel() == RADIOLIB_LORA_DETECTED)
        busy[k]++;
  }

  loraIrq = false;
  applyChannel(curChan);
  return busy[1] < busy[0] ? CH_LONG2 : CH_LONG1;
}

// tell the watch first (if it hears us), then change
void startMove(int ch)
{
  if(ch == curChan || moveQueued || moveAfterTx)
    return;

  if(watchLastSeen != 0 && millis() - watchLastSeen < 6000)
  {
    moveTo = ch;
    moveQueued = true;
  }
  else
  {
    applyChannel(ch);
  }
}

// AUTO: fast first; long range when the watch hears us badly or not at all;
// back to fast when it hears us strongly for a minute
void autoChannel(unsigned long now)
{
  if(chanMode != CHM_AUTO)
  {
    int want = chanMode == CHM_FAST ? CH_FAST : chanMode == CHM_LONG1 ? CH_LONG1 : CH_LONG2;

    if(curChan != want)
      startMove(want);

    return;
  }

  bool fresh = watchReportAt != 0 && now - watchReportAt < 12000 && watchReportAt > chanSince;
  unsigned long heard = max(watchLastSeen, chanSince);
  unsigned long quiet = now - heard;

  if(curChan == CH_FAST)
  {
    bool weak = fresh && (watchHearsRodRssi < -105 || watchHearsRodSnr < -3 || watchLoss > 35);

    if(weak || quiet > 12000)
    {
      if(weak)
        Serial.printf("[LoRa] the watch hears us badly (%d dBm, SNR %d, %d%% missed) -> long range\n", watchHearsRodRssi, watchHearsRodSnr, watchLoss);
      else
        Serial.println("[LoRa] the watch is silent for 12 s -> long range");

      startMove(quieterLong());
    }
  }
  else
  {
    bool strong = fresh && watchHearsRodRssi > -85 && watchHearsRodSnr > 5 && watchLoss < 10;

    if(!strong)
      strongSince = 0;
    else if(strongSince == 0)
      strongSince = now;

    if((strong && now - strongSince > 60000) || quiet > 30000)
    {
      Serial.println(quiet > 30000 ? "[LoRa] the watch is silent for 30 s -> try the fast channel" : "[LoRa] the watch hears us strongly for 1 min -> fast channel");
      startMove(CH_FAST);
    }
  }
}

// /lorachan?m=auto|fast|long1|long2 (app or watch)
void setChanMode(int m)
{
  chanMode = constrain(m, (int)CHM_AUTO, (int)CHM_LONG2);

  Preferences p;
  p.begin("strikesense", false);
  p.putInt("lchm", chanMode);
  p.end();
}

void handleLoraChan()
{
  String m = server.arg("m");
  int v = m == "auto" ? CHM_AUTO : m == "fast" ? CHM_FAST : m == "long1" ? CHM_LONG1 : m == "long2" ? CHM_LONG2 : -1;

  if(v < 0)
  {
    server.send(400, "text/plain", "BAD CHANNEL");
    return;
  }

  setChanMode(v);
  server.send(200, "text/plain", "OK");
}

// The watch gets the rod's LoRa key over the encrypted Bluetooth pairing
void handleLoraKey()
{
  char hex[33];

  for(int i = 0; i < 16; i++)
    snprintf(hex + 2 * i, 3, "%02x", loraKey[i]);

  String json = "{\"id\":\"" + rodId + "\",\"tag\":" + String(rodTag) + ",\"key\":\"" + String(hex) + "\"}";
  server.send(200, "application/json", json);
}

// ---------- receiving ----------

void handleCommand(ss_command_t c)
{
  // Always confirm, even a repeat (our first ACK may have been lost)
  queueAck(PKT_COMMAND, c.h.seq);

  if(c.h.seq == lastCommandSeq)
    return;

  lastCommandSeq = c.h.seq;

  Serial.printf("[LoRa] command %d from watch\n", c.cmd);

  switch(c.cmd)
  {
    case CMD_LEARN:    cmdRelearn(true); break;
    case CMD_WATCHING: cmdWatching(); break;
    case CMD_REELIN:   cmdReelIn(); break;
    case CMD_READY:    cmdReady(); break;
    case CMD_RESUME:   cmdResume(); break;
    case CMD_SKIPPULL: cmdSkipPull(); break;
    case CMD_TEACHDIR: cmdTeachDir(); break;
    case CMD_RELEARN:  cmdRelearn(false); break;
    case CMD_SETTIME:  setClock((long)c.arg, (long)c.tz15 * 15); break;
  }
}

// a packet from our own watch, already opened
void handlePacket(const uint8_t* buf, size_t len)
{
  if(len < sizeof(ss_header_t))
    return;

  const ss_header_t* h = (const ss_header_t*)buf;

  if(h->net != SS_NET_ID)
    return;

  watchLastSeen = millis();
  watchRssi = radio.getRSSI();
  watchSnr = radio.getSNR();

  if(h->type == PKT_WATCH_HB && len >= sizeof(ss_watch_hb_t) - 1)
  {
    ss_watch_hb_t w;
    memset(&w, 0, sizeof(w));
    memcpy(&w, buf, len < sizeof(w) ? len : sizeof(w));

    watchHearsRodRssi = w.rodRssi;
    watchHearsRodSnr = w.rodSnr;
    watchLoss = w.loss;
    watchReportAt = millis();
    watchReported = true;
  }
  else if(h->type == PKT_COMMAND && len >= SS_COMMAND_V1_LEN)
  {
    ss_command_t c;
    memset(&c, 0, sizeof(c));
    memcpy(&c, buf, len < sizeof(c) ? len : sizeof(c));     // an older watch sends a shorter one
    handleCommand(c);
  }
  else if(h->type == PKT_ACK && len >= sizeof(ss_ack_t))
  {
    const ss_ack_t* a = (const ss_ack_t*)buf;

    if(a->ackType == PKT_STRIKE && a->ackValue == strikeAckId)
    {
      strikeNeedsAck = false;
      Serial.println("[LoRa] watch confirmed the strike");
    }
  }
}

// anything on the air: ours (open it) or another rod's (count it)
void handleRaw(const uint8_t* buf, size_t len)
{
  if(len < SS_WIRE_OVERHEAD + sizeof(ss_header_t) || buf[0] != SS_MAGIC)
    return;

  ss_wire_t w;
  memcpy(&w, buf, sizeof(w));
  size_t n = len - SS_WIRE_OVERHEAD;

  uint8_t mac[16];
  ss_aes_cmac(&loraAes, buf, sizeof(w) + n, mac);

  if(w.rod != rodTag || memcmp(mac, buf + sizeof(w) + n, SS_MAC_LEN) != 0)
  {
    noteOtherRod(w.rod, loraAirtimeMs(len));      // not ours: only counted (crowd)
    return;
  }

  if(w.from != 1 || (lastWatchCtr != 0 && w.ctr <= lastWatchCtr) || n > 64)
    return;                                       // not from our watch, or a copy of an old packet

  lastWatchCtr = w.ctr;

  uint8_t plain[64];
  memcpy(plain, buf + sizeof(w), n);

  uint8_t nonce[16] = { 0 };
  memcpy(nonce, &w, sizeof(w));
  ss_aes_ctr(&loraAes, nonce, plain, n);

  handlePacket(plain, n);
}

void loraBegin()
{
  SPI.begin(ROD_LORA_SCK, ROD_LORA_MISO, ROD_LORA_MOSI, ROD_LORA_CS);

  loraKeyBegin();

  int ch = chanMode == CHM_LONG1 ? CH_LONG1 : chanMode == CHM_LONG2 ? CH_LONG2 : CH_FAST;
  const ss_channel_t& c = SS_CHANNELS[ch];

  // Wio-SX1262: 1.8V TCXO powered from DIO3
  int16_t state = radio.begin(c.mhz, c.khz, c.sf, SS_CR, SS_SYNC_WORD, c.dbm, SS_PREAMBLE, 1.8);

  if(state != RADIOLIB_ERR_NONE)
  {
    Serial.printf("[LoRa] FAILED, error %d - running without the watch\n", state);
    return;
  }

  // Wio-SX1262 antenna switch is driven by DIO2
  radio.setDio2AsRfSwitch(true);

  radio.setDio1Action(onLoraIrq);
  radio.startReceive();

  curChan = ch;
  chanSince = millis();
  dutyAt = millis();
  loraOk = true;

  Serial.printf("[LoRa] OK  %s  tag %04X  encrypted - talking to the watch\n", rodId.c_str(), rodTag);
}

void loraPoll()
{
  if(!loraOk)
    return;

  unsigned long now = millis();

  // the 10% rule: 0.1 ms of airtime earned per ms, up to 20 s saved
  dutyTokens = min(20000.0f, dutyTokens + (now - dutyAt) * 0.1f);
  dutyAt = now;

  // other rods' airtime fades over ~10 s; crowd mode on / off
  if(now - otherAirAt >= 100)
  {
    otherAirMs *= powf(0.99f, (now - otherAirAt) / 100.0f);
    otherAirAt = now;

    int n = othersCount();
    float load = otherAirMs / 10000.0f;

    if(!crowd && (n >= 6 || load >= 0.25f))
    {
      crowd = true;
      Serial.printf("[LoRa] crowd mode ON (%d rods near)\n", n);
    }
    else if(crowd && n <= 3 && load < 0.15f)
    {
      crowd = false;
      Serial.println("[LoRa] crowd mode off");
    }
  }

  // ---------- Radio finished sending or received something ----------

  if(loraIrq)
  {
    loraIrq = false;

    if(loraTxBusy)
    {
      radio.finishTransmit();
      loraTxBusy = false;

      if(moveAfterTx)
      {
        moveAfterTx = false;
        applyChannel(moveTo);
      }
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

  // Never stay stuck "sending"
  if(loraTxBusy && now - loraTxStart > 2000)
  {
    loraTxBusy = false;
    radio.startReceive();
  }

  if(loraTxBusy)
    return;

  // ---------- What to send next (most important first) ----------

  if(ackQueued && (long)(now - ackDueAt) >= 0)
  {
    if(loraSend(&queuedAck, sizeof(queuedAck), true))
      ackQueued = false;

    return;
  }

  if(linkMode == 1)
    return;                        // Bluetooth only: the rod's LoRa stays quiet

  if((long)(now - loraBackoffUntil) < 0)
    return;                        // someone else is talking: wait our turn

  if(moveQueued)
  {
    ss_move_t m;
    fillHeader(m.h, PKT_MOVE);
    m.chan = (uint8_t)moveTo;

    if(loraSend(&m, sizeof(m), true))
    {
      moveQueued = false;
      moveAfterTx = true;
    }

    return;
  }

  if(strikeNeedsAck && strikeActive && (long)(now - nextStrikeSend) >= 0)
  {
    if(!sendStrike())
      return;

    strikeTries++;

    // the first 2 s every 0.3 s, then every second
    nextStrikeSend = now + (now - strikeFirstSent < 2000 ? 300 : SS_RESEND_MS) + random(0, 100);

    if(strikeTries >= SS_MAX_TRIES + 6)
      strikeNeedsAck = false;

    return;
  }

  if(now - lastHeartbeatSent >= (loraBusy() ? busyGapMs() : calmGapMs()))
  {
    if(sendHeartbeat())
      lastHeartbeatSent = now;

    return;
  }

  autoChannel(now);
}

bool loraRadioOk()
{
  return loraOk;
}

// Watch heard in the last 15 s (it reports every 5 s)
bool watchConnected()
{
  return loraOk && watchLastSeen != 0 && millis() - watchLastSeen < 15000;
}

int loraBars(float rssi)
{
  if(rssi > -80)  return 4;
  if(rssi > -95)  return 3;
  if(rssi > -108) return 2;

  return 1;
}

int loraWatchBars()
{
  return loraBars(watchRssi);
}

void addLoraFields(String& json)
{
  bool connected = watchConnected();

  addField(json, "lora", loraOk);
  addField(json, "watch", connected);
  addField(json, "wbars", (long)(connected ? loraBars(watchRssi) : 0));
  addField(json, "wrssi", (long)watchRssi);
  addField(json, "wsnr", watchSnr, 1);
  addField(json, "wrep", connected && watchReported);
  addField(json, "w2rssi", (long)watchHearsRodRssi);
  addField(json, "w2snr", (long)watchHearsRodSnr);
  addField(json, "wseen", watchLastSeen == 0 ? -1L : (long)((millis() - watchLastSeen) / 1000));
}

// the app's watch line: channel, quality, rods near
void addLoraChannel(String& json)
{
  addField(json, "lchan", (long)curChan);
  addField(json, "lchm", (long)chanMode);
  addField(json, "lqual", (long)loraQuality());
  addField(json, "lrods", (long)othersCount());
  addField(json, "crowd", crowd);
}
'''


def patch(path):
    s = open(path).read()

    if "void handleRaw(" in s:
        print(path, "already done")
        return

    def rep(a, b, n=1):
        nonlocal s
        assert s.count(a) == n, (path, s.count(a), a)
        s = s.replace(a, b)

    a = s.index("bool loraOk = false;\n")
    b = s.index("#else\n\nvoid loraOnStrike() {}")
    s = s[:a] + NEW + "\n" + s[b:]

    rep('#include "strikesense_protocol.h"\n', '#include "strikesense_protocol.h"\n#include "ss_crypto.h"\n')

    rep("""void addLoraFields(String& json)
{
  addField(json, "lora", false);
  addField(json, "watch", false);
}
""", """void addLoraFields(String& json)
{
  addField(json, "lora", false);
  addField(json, "watch", false);
}

void addLoraChannel(String&) {}
""")

    # declarations at the top (definitions are in the LoRa part)
    rep("""void loraOnStrike();
void addLoraFields(String& json);""", """void loraOnStrike();
void addLoraFields(String& json);
void addLoraChannel(String& json);
void handleLoraChan();
void handleLoraKey();""")

    rep("""  addLinkMode(json);
""", """  addLinkMode(json);
  addLoraChannel(json);
""")

    rep("""  server.on("/linkmode", handleLinkMode);
""", """  server.on("/linkmode", handleLinkMode);
#if ENABLE_LORA
  server.on("/lorachan", handleLoraChan);
  server.on("/lorakey", handleLoraKey);
#endif
""")

    rep("version 2.3 (LoRa + Bluetooth: watch switches by itself)", "version 2.4 (private LoRa: auto channel, crowd mode)")

    open(path, "w").write(s)
    print(path, "done")


patch(sys.argv[1] if len(sys.argv) > 1 else PATH)

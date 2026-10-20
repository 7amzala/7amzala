#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <string>

#include "ble_link.h"

#if defined(CONFIG_NIMBLE_ENABLED)
#include "host/ble_gap.h"
#endif

// Same as the rod (StrikeSense .ino) and the phone app (RodLink.java)
static BLEUUID SERVICE_UUID("6e8a0001-5f2d-4c8e-9a51-2b7c1d3e4f60");
static BLEUUID RX_UUID("6e8a0002-5f2d-4c8e-9a51-2b7c1d3e4f60");   // watch -> rod
static BLEUUID TX_UUID("6e8a0003-5f2d-4c8e-9a51-2b7c1d3e4f60");   // rod -> watch

static const uint32_t ASK_FAST_MS = 100;         // 10 times a second when something moves
static const uint32_t ASK_CALM_MS = 200;         // 5 times a second when all is calm (less radio for the phone)
static const uint32_t ANSWER_TIMEOUT_MS = 600;   // a lost answer: ask again soon
static const uint32_t COMMAND_TIMEOUT_MS = 2000;
static const int COMMAND_TRIES = 3;
static const long FIRST_ID = 500000;             // the phone uses ids below this

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Shared with the screen task (guarded by mux)
static ble_link_status_t shared;
static float peak = 0;
static uint32_t lastAnswerMs = 0;
static char commandPath[32] = "";
static bool commandBusy = false;

// Notifications from the rod (Bluetooth task) -> loop()
static const size_t RING = 8192;
static uint8_t ring[RING];
static volatile size_t ringHead = 0;
static volatile size_t ringTail = 0;

// Link state (loop() only)
static ble_link_phase_t phase = BLE_LINK_SEARCHING;
static BLEClient *client = nullptr;
static BLERemoteCharacteristic *rx = nullptr;
static BLEAdvertisedDevice *found = nullptr;
static volatile bool lostFlag = false;
static size_t piece = 20;
static std::string inbox;
static long nextId = FIRST_ID;

static long askId = -1;
static uint32_t askedAt = 0;
static uint32_t lastAsk = 0;

static long cmdId = -1;
static uint32_t cmdSentAt = 0;
static int cmdTries = 0;
static char cmdSending[32] = "";

static long lastId = -1;              // "/laststrike" in flight
static uint32_t lastAskedAt = 0;
static volatile bool wantLast = false;
static ble_last_strike_t lastStrike;   // guarded by mux

static uint32_t lastLiveMs = 0;

// Bluetooth link speed: slower while the rod is calm and the signal strong
static bool linkFast = true;
static uint32_t calmSince = 0;          // last live message from the rod (0 = none yet)

static uint32_t lastRssi = 0;
static int rssi = 0;

static int scans = 0;
static volatile int seenCount = 0;
static uint32_t connectedAt = 0;
static bool firstAnswerLogged = false;

// ---------- scanning ----------

// The rod: our service number, or (if that did not come through) its name
static bool isRod(BLEAdvertisedDevice &d)
{
  if(d.haveServiceUUID() && d.isAdvertisingService(SERVICE_UUID))
    return true;

  if(d.haveName())
  {
    String name = d.getName();
    return name.startsWith("StrikeSense") && name != "StrikeSense Watch";
  }

  return false;
}

class AdvCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice d) override
  {
    seenCount = seenCount + 1;

    bool rod = isRod(d);

    // only named devices, so the list stays short
    if(d.haveName() || rod)
      Serial.printf("[BLE]   saw %s \"%s\" %d dBm%s\n", d.getAddress().toString().c_str(),
                    d.haveName() ? d.getName().c_str() : "", d.getRSSI(), rod ? "  <- THE ROD" : "");

    if(found == nullptr && rod)
    {
      found = new BLEAdvertisedDevice(d);
      BLEDevice::getScan()->stop();
    }
  }
};

class ClientCallbacks : public BLEClientCallbacks
{
  void onConnect(BLEClient *) override
  {
  }

  void onDisconnect(BLEClient *) override
  {
    lostFlag = true;
  }
};

static void onNotify(BLERemoteCharacteristic *, uint8_t *data, size_t length, bool)
{
  portENTER_CRITICAL(&mux);

  for(size_t i = 0; i < length; i++)
  {
    size_t next = (ringHead + 1) % RING;

    if(next == ringTail)
      break;                 // full: drop (the answer will be asked again)

    ring[ringHead] = data[i];
    ringHead = next;
  }

  portEXIT_CRITICAL(&mux);
}

// ---------- small helpers ----------

static void setNote(const char *text)
{
  portENTER_CRITICAL(&mux);
  strncpy(shared.note, text, sizeof(shared.note) - 1);
  shared.note[sizeof(shared.note) - 1] = 0;
  portEXIT_CRITICAL(&mux);
}

static long newId()
{
  long id = nextId++;

  if(nextId > 999999)
    nextId = FIRST_ID;

  return id;
}

static bool send(long id, const char *path)
{
  if(rx == nullptr)
    return false;

  char line[64];
  int n = snprintf(line, sizeof(line), "%ld|%s\n", id, path);

  for(int i = 0; i < n; i += piece)
  {
    int len = n - i < (int)piece ? n - i : (int)piece;
    rx->writeValue((uint8_t *)line + i, len, false);
  }

  return true;
}

// Value of "key" in a flat JSON object, as text (quotes removed)
static bool jsonGet(const std::string &j, const char *key, char *out, size_t size)
{
  std::string k = std::string("\"") + key + "\":";
  size_t p = j.find(k);

  if(p == std::string::npos)
    return false;

  p += k.size();

  bool quoted = p < j.size() && j[p] == '"';

  if(quoted)
    p++;

  size_t n = 0;

  while(p < j.size() && n + 1 < size)
  {
    char c = j[p];

    if(quoted ? c == '"' : (c == ',' || c == '}'))
      break;

    out[n++] = c;
    p++;
  }

  out[n] = 0;
  return true;
}

static float jsonNum(const std::string &j, const char *key, float fallback)
{
  char v[24];
  return jsonGet(j, key, v, sizeof(v)) ? atof(v) : fallback;
}

static bool jsonBool(const std::string &j, const char *key)
{
  char v[8];
  return jsonGet(j, key, v, sizeof(v)) && strcmp(v, "true") == 0;
}

// ---------- answers ----------

static void gotWatchData(const std::string &j)
{
  ble_link_status_t s;

  portENTER_CRITICAL(&mux);
  s = shared;
  portEXIT_CRITICAL(&mux);

  jsonGet(j, "state", s.state, sizeof(s.state));
  jsonGet(j, "type", s.type, sizeof(s.type));
  s.strike = jsonBool(j, "strike");
  s.strikeid = (long)jsonNum(j, "strikeid", 0);
  s.knockid = (long)jsonNum(j, "knockid", 0);
  s.knockthr = jsonNum(j, "knockthr", 0.01);
  s.tilt = jsonNum(j, "tilt", 0);
  s.allow = jsonNum(j, "allow", 1);
  s.ptdeg = jsonNum(j, "ptdeg", 0);
  s.ptneed = jsonNum(j, "ptneed", 1);
  s.bucket = (int)jsonNum(j, "bucket", 0);
  s.drop = (int)jsonNum(j, "drop", 0);
  s.knocks = (int)jsonNum(j, "knocks", 0);
  s.wave = (int)jsonNum(j, "wave", 0);
  s.wind = (int)jsonNum(j, "wind", 0);
  s.bat = (int)jsonNum(j, "bat", -1);
  s.batleft = (long)jsonNum(j, "batleft", -1);
  s.dir = jsonBool(j, "dir");
  s.dropwatch = jsonBool(j, "dropwatch");

  float e = jsonNum(j, "energy", 0);

  portENTER_CRITICAL(&mux);
  s.seq = shared.seq + 1;
  s.phase = shared.phase;
  s.rssi = shared.rssi;
  s.energy_peak = shared.energy_peak;
  s.command_busy = shared.command_busy;
  shared = s;

  if(e > peak)
    peak = e;

  lastAnswerMs = millis();
  portEXIT_CRITICAL(&mux);
}

// "k":[1,2,3] -> numbers
static int jsonList(const std::string &j, const char *key, short *out, int max)
{
  std::string k = std::string("\"") + key + "\":[";
  size_t p = j.find(k);

  if(p == std::string::npos)
    return 0;

  const char *c = j.c_str() + p + k.size();
  int n = 0;

  while(*c && *c != ']' && n < max)
  {
    char *end;
    long v = strtol(c, &end, 10);

    if(end == c)
      break;

    out[n++] = (short)v;
    c = end;

    if(*c == ',')
      c++;
  }

  return n;
}

static void gotLastStrike(const std::string &j)
{
  static ble_last_strike_t ls;       // big: not on the stack

  memset(&ls, 0, sizeof(ls));
  ls.loaded = true;
  ls.have = jsonBool(j, "have");
  ls.recording = jsonBool(j, "recording");
  jsonGet(j, "type", ls.type, sizeof(ls.type));
  ls.ago_s = (long)jsonNum(j, "ago", 0);
  ls.got_ms = millis();
  ls.count = (int)jsonNum(j, "count", 0);
  ls.tiltpeak = jsonNum(j, "tiltpeak", 0);
  ls.pull = (int)jsonNum(j, "pull", 0);
  ls.pre = (int)jsonNum(j, "pre", 0);
  ls.n = jsonList(j, "k", ls.k, LS_MAX);

  int nt = jsonList(j, "t", ls.t, LS_MAX);

  if(nt < ls.n)
    ls.n = nt;

  // ZOOM: 2 s before + 2 s after at 50 a second
  ls.zpre = (int)jsonNum(j, "zpre", 0);
  ls.zn = jsonList(j, "zk", ls.zk, LS_ZOOM);

  int zt = jsonList(j, "zt", ls.zt, LS_ZOOM);

  if(zt < ls.zn)
    ls.zn = zt;

  portENTER_CRITICAL(&mux);
  lastStrike = ls;
  portEXIT_CRITICAL(&mux);
}

static void answer(long id, int code, const std::string &body)
{
  if(id == lastId)
  {
    lastId = -1;
    Serial.printf("[BLE] /laststrike -> %d, %u bytes\n", code, (unsigned)body.size());

    if(code == 200)
    {
      gotLastStrike(body);
    }
    else
    {
      portENTER_CRITICAL(&mux);
      lastStrike.loaded = true;
      lastStrike.error = code;           // 404: the rod sketch has no last strike yet
      portEXIT_CRITICAL(&mux);
    }

    return;
  }

  if(id == askId)
  {
    askId = -1;

    if(code == 200)
    {
      gotWatchData(body);

      if(!firstAnswerLogged)
      {
        firstAnswerLogged = true;
        Serial.printf("[BLE] live data OK: %s\n", body.c_str());
      }
    }
    else if(!firstAnswerLogged)
    {
      // 404: the rod sketch is older than the watch (no "/watch")
      firstAnswerLogged = true;
      Serial.printf("[BLE] rod answered %d to /watch - update the rod sketch\n", code);
      char note[40];
      snprintf(note, sizeof(note), "rod sketch too old (%d) - update it", code);
      setNote(note);
    }
  }
  else if(id == cmdId)
  {
    Serial.printf("[BLE] %s -> %s\n", cmdSending, body.c_str());

    cmdId = -1;
    cmdSending[0] = 0;

    portENTER_CRITICAL(&mux);
    commandBusy = false;
    portEXIT_CRITICAL(&mux);
  }
}

// Answers start with "#<id> <code> <length>\n" and may come in pieces.
// The phone's answers arrive here too: they are skipped.
static void readAnswers()
{
  while(true)
  {
    uint8_t buf[512];
    size_t n = 0;

    portENTER_CRITICAL(&mux);

    while(ringTail != ringHead && n < sizeof(buf))
    {
      buf[n++] = ring[ringTail];
      ringTail = (ringTail + 1) % RING;
    }

    portEXIT_CRITICAL(&mux);

    if(n == 0)
      break;

    inbox.append((const char *)buf, n);
  }

  while(true)
  {
    size_t start = inbox.find('#');

    if(start == std::string::npos)
    {
      inbox.clear();
      return;
    }

    size_t nl = inbox.find('\n', start);

    if(nl == std::string::npos)
    {
      inbox.erase(0, start);

      if(inbox.size() > 64)       // no header end: rubbish, skip the '#'
        inbox.erase(0, 1);

      return;
    }

    long id = 0;
    int code = 0;
    long len = 0;

    // the rod's live message (10 a second, for everyone): "#live 200 <len>"
    bool live = inbox.compare(start + 1, 5, "live ") == 0;

    if(live)
    {
      if(sscanf(inbox.c_str() + start + 6, "%d %ld", &code, &len) != 2 || len < 0 || len > 4000)
      {
        inbox.erase(0, nl + 1);
        continue;
      }

      if(inbox.size() - (nl + 1) < (size_t)len)
      {
        inbox.erase(0, start);      // wait for the rest
        return;
      }

      std::string body = inbox.substr(nl + 1, len);
      inbox.erase(0, nl + 1 + len);

      if(code == 200)
      {
        gotWatchData(body);
        lastLiveMs = millis();

        if(!firstAnswerLogged)
        {
          firstAnswerLogged = true;
          Serial.printf("[BLE] live data OK (rod sends it by itself): %s\n", body.c_str());
        }
      }

      continue;
    }

    if(sscanf(inbox.c_str() + start + 1, "%ld %d %ld", &id, &code, &len) != 3 || len < 0 || len > 200000)
    {
      inbox.erase(0, nl + 1);
      continue;
    }

    if(inbox.size() - (nl + 1) < (size_t)len)
    {
      inbox.erase(0, start);      // wait for the rest
      return;
    }

    std::string body = inbox.substr(nl + 1, len);
    inbox.erase(0, nl + 1 + len);

    if(id >= FIRST_ID)
      answer(id, code, body);
  }
}

// ---------- connecting ----------

static void setPhase(ble_link_phase_t p)
{
  phase = p;

  portENTER_CRITICAL(&mux);
  shared.phase = p;

  if(p != BLE_LINK_CONNECTED)
    shared.rssi = 0;

  portEXIT_CRITICAL(&mux);
}

static void lost()
{
  Serial.println("[BLE] rod lost, searching again");

  if(client != nullptr)
  {
    if(client->isConnected())
      client->disconnect();

    delete client;
    client = nullptr;
  }

  rx = nullptr;
  askId = -1;
  cmdId = -1;
  lastId = -1;
  lastLiveMs = 0;
  inbox.clear();

  portENTER_CRITICAL(&mux);
  ringTail = ringHead;
  portEXIT_CRITICAL(&mux);

  setPhase(BLE_LINK_SEARCHING);
}

// fast: 7.5-15 ms; calm: 90-110 ms (the live message comes every 100 ms anyway)
static void setLinkFast(bool fast)
{
  if(fast == linkFast || client == nullptr)
    return;

  linkFast = fast;

#if defined(CONFIG_NIMBLE_ENABLED)
  ble_gap_upd_params p = {};
  p.itvl_min = fast ? 6 : 72;          // x 1.25 ms
  p.itvl_max = fast ? 12 : 88;
  p.latency = 0;
  p.supervision_timeout = 400;         // x 10 ms
  ble_gap_update_params(client->getConnId(), &p);
#endif

  Serial.printf("[BLE] link %s\n", fast ? "fast (something moves, or weak signal)" : "calm (saves power)");
}

static void connectToRod()
{
  setPhase(BLE_LINK_CONNECTING);
  lostFlag = false;

  client = BLEDevice::createClient();
  client->setClientCallbacks(new ClientCallbacks());

  Serial.printf("[BLE] rod found (%s), connecting...\n", found->getAddress().toString().c_str());
  setNote("rod found - connecting...");

  bool ok = client->connect(found);

  delete found;
  found = nullptr;

  if(!ok)
  {
    Serial.println("[BLE] could not connect");
    lost();
    setNote("rod found but could not connect");
    return;
  }

  BLERemoteService *service = client->getService(SERVICE_UUID);
  BLERemoteCharacteristic *tx = service != nullptr ? service->getCharacteristic(TX_UUID) : nullptr;

  rx = service != nullptr ? service->getCharacteristic(RX_UUID) : nullptr;

  if(rx == nullptr || tx == nullptr || !tx->canNotify())
  {
    Serial.println("[BLE] connected, but no StrikeSense service (old rod sketch?)");
    lost();
    setNote("no StrikeSense service on rod");
    return;
  }

  tx->registerForNotify(onNotify);

  linkFast = true;
  calmSince = 0;

  // Fast link, like the phone app (7.5-15 ms instead of 20-40 ms)
#if defined(CONFIG_NIMBLE_ENABLED)
  ble_gap_upd_params fast = {};
  fast.itvl_min = 6;                 // x 1.25 ms
  fast.itvl_max = 12;
  fast.latency = 0;
  fast.supervision_timeout = 400;    // x 10 ms
  int rc = ble_gap_update_params(client->getConnId(), &fast);
  Serial.printf("[BLE] fast link %s\n", rc == 0 ? "asked" : "not possible");
#endif

  // The size exchange may still be on its way
  uint32_t waitMtu = millis();

  while(client->getMTU() <= 23 && millis() - waitMtu < 1000)
    delay(10);

  int mtu = client->getMTU();
  piece = mtu > 23 ? mtu - 3 : 20;

  if(piece > 512)
    piece = 512;

  // Tell the rod how big a piece may be (it uses the smallest of phone/watch)
  char path[24];
  snprintf(path, sizeof(path), "/mtu?v=%u", (unsigned)piece);
  send(newId(), path);

  setPhase(BLE_LINK_CONNECTED);
  connectedAt = millis();
  firstAnswerLogged = false;
  setNote("connected - waiting for data");
  Serial.printf("[BLE] connected to the rod (MTU %d)\n", mtu);
}

// ---------- public ----------

void ble_link_begin(void)
{
  memset(&shared, 0, sizeof(shared));
  shared.bat = -1;
  shared.batleft = -1;
  strcpy(shared.state, "");

  BLEDevice::init("StrikeSense Watch");
  BLEDevice::setMTU(517);

  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new AdvCallbacks());
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

  Serial.println("[BLE] looking for the rod...");
}

void ble_link_poll(void)
{
  uint32_t now = millis();

  if(phase == BLE_LINK_SEARCHING)
  {
    seenCount = 0;
    BLEDevice::getScan()->start(3, false);       // up to 3 s, stops early when found
    BLEDevice::getScan()->clearResults();
    scans++;

    portENTER_CRITICAL(&mux);
    shared.scans = scans;
    shared.seen = seenCount;
    portEXIT_CRITICAL(&mux);

    if(found != nullptr)
      connectToRod();
    else
      Serial.printf("[BLE] scan %d: %d devices, no rod\n", scans, (int)seenCount);

    return;
  }

  if(phase != BLE_LINK_CONNECTED)
    return;

  if(lostFlag || client == nullptr || !client->isConnected())
  {
    lost();
    setNote("rod lost - searching again");
    return;
  }

  readAnswers();

  if(!firstAnswerLogged && now - connectedAt > 5000)
  {
    firstAnswerLogged = true;
    Serial.println("[BLE] connected, but the rod does not answer /watch (old rod sketch?)");
    setNote("connected, rod not answering");
  }

  // live data
  if(askId >= 0 && now - askedAt > ANSWER_TIMEOUT_MS)
    askId = -1;

  // something going on (knock, pull, drop-back, strike, learning...): ask fast
  bool busy;

  portENTER_CRITICAL(&mux);
  busy = shared.strike || shared.bucket >= 5 || shared.drop >= 5 || shared.dropwatch ||
         shared.knocks > 0 || commandBusy || strcmp(shared.state, "monitoring") != 0;
  portEXIT_CRITICAL(&mux);

  // calm rod + strong signal for 10 s: slower link (saves power); anything else: fast at once
  bool strong = rssi != 0 && rssi > -75;

  if(busy || !strong || lastId >= 0 || wantLast)
  {
    calmSince = 0;
    setLinkFast(true);
  }
  else
  {
    if(calmSince == 0)
      calmSince = now;

    if(now - calmSince > 10000)
      setLinkFast(false);
  }

  // the rod sends live messages by itself: no need to ask (older rods: ask)
  bool liveOn = lastLiveMs != 0 && now - lastLiveMs < 1000;

  if(!liveOn && askId < 0 && now - lastAsk >= (busy ? ASK_FAST_MS : ASK_CALM_MS))
  {
    askId = newId();
    askedAt = now;
    lastAsk = now;
    send(askId, "/watch");
  }

  // LAST STRIKE page asked for the rod's last strike
  if(lastId >= 0 && now - lastAskedAt > 3000)
  {
    Serial.println("[BLE] /laststrike: no answer in 3 s, asking again");
    lastId = -1;                          // no answer: may ask again
    wantLast = true;
  }

  if(wantLast && lastId < 0)
  {
    wantLast = false;
    lastId = newId();
    lastAskedAt = now;
    send(lastId, "/laststrike");
  }

  // buttons: sent until the rod answers (3 tries)
  char path[32] = "";

  portENTER_CRITICAL(&mux);

  if(commandPath[0] != 0)
  {
    strcpy(path, commandPath);
    commandPath[0] = 0;
  }

  portEXIT_CRITICAL(&mux);

  if(path[0] != 0)
  {
    strcpy(cmdSending, path);
    cmdTries = 0;
    cmdId = -1;
  }

  if(cmdSending[0] != 0 && cmdId < 0 && cmdTries == 0)
  {
    cmdId = newId();
    cmdSentAt = now;
    cmdTries = 1;
    send(cmdId, cmdSending);
  }
  else if(cmdSending[0] != 0 && cmdId >= 0 && now - cmdSentAt > COMMAND_TIMEOUT_MS)
  {
    if(cmdTries < COMMAND_TRIES)
    {
      cmdId = newId();
      cmdSentAt = now;
      cmdTries++;
      send(cmdId, cmdSending);
    }
    else
    {
      Serial.printf("[BLE] %s: no answer\n", cmdSending);
      cmdSending[0] = 0;
      cmdId = -1;

      portENTER_CRITICAL(&mux);
      commandBusy = false;
      portEXIT_CRITICAL(&mux);
    }
  }

  // signal strength every 2 s
  if(now - lastRssi > 2000)
  {
    lastRssi = now;
    rssi = client->getRssi();

    portENTER_CRITICAL(&mux);
    shared.rssi = rssi;
    portEXIT_CRITICAL(&mux);
  }
}

void ble_link_get(ble_link_status_t *out, bool take_peak)
{
  uint32_t now = millis();

  portENTER_CRITICAL(&mux);
  *out = shared;
  out->energy_peak = peak;
  out->age_ms = lastAnswerMs == 0 ? 0xFFFFFFFF : now - lastAnswerMs;
  out->command_busy = commandBusy;

  if(take_peak)
    peak = 0;

  portEXIT_CRITICAL(&mux);
}

void ble_link_ask_last_strike(void)
{
  wantLast = true;
}

bool ble_link_get_last_strike(ble_last_strike_t *out)
{
  portENTER_CRITICAL(&mux);
  *out = lastStrike;
  portEXIT_CRITICAL(&mux);

  return out->loaded;
}

void ble_link_command(const char *path)
{
  portENTER_CRITICAL(&mux);
  strncpy(commandPath, path, sizeof(commandPath) - 1);
  commandPath[sizeof(commandPath) - 1] = 0;
  commandBusy = true;
  portEXIT_CRITICAL(&mux);
}

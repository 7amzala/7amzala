#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLESecurity.h>
#include <Preferences.h>
#include <string>
#include <functional>
#include <esp_task_wdt.h>

#include "ble_link.h"
#include "link_merge.h"

extern "C" void pair_restart(void);     // StrikeSense_Watch.ino

#if defined(CONFIG_NIMBLE_ENABLED)
#include "host/ble_gap.h"
#include "host/ble_store.h"
#elif defined(CONFIG_BLUEDROID_ENABLED)
#include <esp_gap_ble_api.h>
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
static char commandPath[48] = "";
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
static long keyId = -1;               // "/lorakey" in flight (our rod's LoRa key)
static uint32_t lastAskedAt = 0;
static volatile bool wantLast = false;
static ble_last_strike_t lastStrike;   // guarded by mux

static uint32_t lastLiveMs = 0;
static uint32_t lastPingMs = 0;         // "/wping": tells the rod (and the phone) the watch is here

// Bluetooth link speed: slower while the rod is calm and the signal strong
static bool linkFast = true;
static uint32_t calmSince = 0;          // last live message from the rod (0 = none yet)

static uint32_t lastRssi = 0;
static int rssi = 0;

static int scans = 0;
static uint32_t scanEndMs = 0;
static const uint32_t SCAN_PAUSE_LORA_MS = 4000;  // on LoRa: look for Bluetooth less often (battery)
static volatile int seenCount = 0;
static uint32_t connectedAt = 0;
static bool firstAnswerLogged = false;

// ---------- pairing: our own rod only ----------

static String pairedAddr;               // our rod's Bluetooth address, "" = not paired
static String pairedId;                 // ROD-4F2A

// rods open for pairing nearby (scan task -> screen task, under mux)
struct PairSeen
{
  char id[12];
  char addr[20];
  int rssi;
  uint32_t at;
};

static PairSeen pairSeen[PAIR_MAX];
static int pairSeenN = 0;
static volatile bool pairWanted = false;
static String pairWantAddr;
static char pairWantId[12];
static uint32_t pairPin = 0;
static volatile pair_state_t pairState = PAIR_IDLE;
static int authFails = 0;               // our rod refused us N times in a row (it forgot us)

// the PIN check (Bluetooth security)
static volatile bool authDone = false;
static volatile bool authOk = false;

class WatchSecurity : public BLESecurityCallbacks
{
  uint32_t onPassKeyRequest() override { return pairPin; }      // the PIN typed on the watch
  void onPassKeyNotify(uint32_t) override {}
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t) override { return true; }

#if defined(CONFIG_BLUEDROID_ENABLED)
  void onAuthenticationComplete(esp_ble_auth_cmpl_t d) override
  {
    authOk = d.success;
    authDone = true;
  }
#endif

#if defined(CONFIG_NIMBLE_ENABLED)
  void onAuthenticationComplete(ble_gap_conn_desc *desc, int status) override
  {
    authOk = status == 0 && desc != nullptr && desc->sec_state.encrypted;
    authDone = true;
  }
#endif
};

// "LoRa only" (chosen in the app or on the watch), kept in flash: after a
// restart the watch does not start a Bluetooth connection before the
// rod's first LoRa heartbeat says it again
static bool savedLoraOnly = false;

static void loadPairing()
{
  Preferences p;
  p.begin("pair", true);
  pairedAddr = p.getString("addr", "");
  pairedId = p.getString("id", "");
  savedLoraOnly = p.getBool("loraonly", false);
  p.end();
}

static void rememberLoraOnly(bool on)
{
  if(on == savedLoraOnly)
    return;

  savedLoraOnly = on;

  Preferences p;
  p.begin("pair", false);
  p.putBool("loraonly", on);
  p.end();
}

static void savePairing(const String &addr, const String &id)
{
  Preferences p;
  p.begin("pair", false);
  p.putString("addr", addr);
  p.putString("id", id);
  p.end();

  pairedAddr = addr;
  pairedId = id;
}

// ---------- scanning ----------

// Paired: only our rod's address. Not paired: nothing (the pairing list fills).
static bool isRod(BLEAdvertisedDevice &d)
{
  return pairedAddr.length() > 0 && d.getAddress().toString().equalsIgnoreCase(pairedAddr);
}

// a rod open for pairing: "ROD-xxxx" (and the StrikeSense service)
static void notePairable(BLEAdvertisedDevice &d)
{
  if(!d.haveName())
    return;

  String name = d.getName();

  if(!name.startsWith("ROD-") || name.length() > 10)
    return;

  String addr = d.getAddress().toString();

  portENTER_CRITICAL(&mux);

  int at = -1;

  for(int i = 0; i < pairSeenN; i++)
    if(strcmp(pairSeen[i].addr, addr.c_str()) == 0)
      at = i;

  if(at < 0 && pairSeenN < PAIR_MAX)
    at = pairSeenN++;

  if(at >= 0)
  {
    strncpy(pairSeen[at].id, name.c_str(), sizeof(pairSeen[at].id) - 1);
    pairSeen[at].id[sizeof(pairSeen[at].id) - 1] = 0;
    strncpy(pairSeen[at].addr, addr.c_str(), sizeof(pairSeen[at].addr) - 1);
    pairSeen[at].addr[sizeof(pairSeen[at].addr) - 1] = 0;
    pairSeen[at].rssi = d.getRSSI();
    pairSeen[at].at = millis();
  }

  portEXIT_CRITICAL(&mux);
}

class AdvCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice d) override
  {
    seenCount = seenCount + 1;

    bool rod = isRod(d);

    if(pairedAddr.length() == 0)
      notePairable(d);

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
  s.ftilt = jsonNum(j, "ft", s.tilt);           // older rod: the normal tilt
  s.time = (long)jsonNum(j, "time", 0);         // UTC seconds, 0 = the rod does not know
  s.tz = (int)jsonNum(j, "tz", 0);
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

  char lm[8] = "";
  jsonGet(j, "lmode", lm, sizeof(lm));
  s.lmode = strcmp(lm, "auto") == 0 ? 0 : strcmp(lm, "bt") == 0 ? 1 : strcmp(lm, "lora") == 0 ? 2 : -1;

  if(s.lmode >= 0)
    rememberLoraOnly(s.lmode == 2);
  s.chm = (int)jsonNum(j, "lchm", -1);

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
  // our rod's LoRa key (only over the encrypted pairing): {"id":"ROD-4F2A","tag":20266,"key":"..32 hex.."}
  if(id == keyId)
  {
    keyId = -1;
    char hex[40] = "";

    if(code == 200 && jsonGet(body, "key", hex, sizeof(hex)) && strlen(hex) == 32)
    {
      uint8_t key[16];

      for(int i = 0; i < 16; i++)
      {
        char b[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        key[i] = (uint8_t)strtol(b, nullptr, 16);
      }

      lora_link_set_key((uint16_t)jsonNum(body, "tag", 0), key);
    }

    return;
  }

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

// "LoRa only" chosen in the phone app (the rod says it in its LoRa heartbeat).
// fresh_ms: how recent that heartbeat must be. Leaving Bluetooth needs a
// heartbeat right now; staying off Bluetooth needs one within 1 min, so a
// silent LoRa brings Bluetooth back by itself.
static bool loraOnly(uint32_t fresh_ms)
{
#if WATCH_LORA
  lora_link_status_t lo = lora_link_get_status();

  if(lo.last_hb_ms != 0 && millis() - lo.last_hb_ms < fresh_ms)
  {
    bool on = (lo.flags & SS_F_LORA_ONLY) != 0;
    rememberLoraOnly(on);
    return on;
  }

  // nothing from the rod's LoRa lately: just after a start, the choice from
  // before (its first heartbeat comes within seconds); later: Bluetooth again
  return savedLoraOnly && millis() < 30000;
#else
  (void)fresh_ms;
  return false;
#endif
}

// Ask the rod's link for fast (7.5-15 ms) or calm (90-110 ms) timing
static void askLinkTiming(bool fast)
{
#if defined(CONFIG_NIMBLE_ENABLED)
  ble_gap_upd_params p = {};
  p.itvl_min = fast ? 6 : 24;          // x 1.25 ms
  p.itvl_max = fast ? 12 : 40;
  p.latency = 0;
  p.supervision_timeout = 400;         // x 10 ms
  ble_gap_update_params(client->getConnId(), &p);
#elif defined(CONFIG_BLUEDROID_ENABLED)
  esp_ble_conn_update_params_t p = {};
  memcpy(p.bda, *client->getPeerAddress().getNative(), sizeof(esp_bd_addr_t));
  p.min_int = fast ? 6 : 24;           // x 1.25 ms
  p.max_int = fast ? 12 : 40;
  p.latency = 0;
  p.timeout = 400;                     // x 10 ms
  esp_ble_gap_update_conn_params(&p);
#endif
}

// fast: 7.5-15 ms; calm: 30-50 ms (was 90-110: a strike then reached the
// watch up to ~0.2 s after the phone; now both hear it together)
static void setLinkFast(bool fast)
{
  if(fast == linkFast || client == nullptr)
    return;

  linkFast = fast;
  askLinkTiming(fast);

  Serial.printf("[BLE] link %s\n", fast ? "fast (something moves, or weak signal)" : "calm (saves power)");
}

// A Bluetooth library call that waits for the rod (encryption, finding its
// service) can wait forever when the rod does not answer: the watchdog then
// restarted the watch, again and again. So it runs in a helper task: we wait
// (telling the watchdog we're alive), and after 'ms' drop the link, which
// ends the call. Still stuck 5 s later (never seen): restart the watch.
struct BleJob
{
  std::function<bool()> fn;
  volatile int result;                  // -1 running, 0 failed, 1 ok
};

static void bleJobTask(void *arg)
{
  BleJob *j = (BleJob *)arg;
  j->result = j->fn() ? 1 : 0;
  vTaskDelete(NULL);
}

static bool jobTimedOut = false;        // the last runWithDeadline gave up waiting

static bool runWithDeadline(std::function<bool()> fn, uint32_t ms, const char *what)
{
  jobTimedOut = false;

  BleJob *j = new BleJob{ fn, -1 };

  if(xTaskCreate(bleJobTask, "ble_job", 6144, j, 2, nullptr) != pdPASS)
  {
    delete j;
    return fn();                        // (no memory for a task: the old way)
  }

  uint32_t t0 = millis();

  while(j->result < 0 && millis() - t0 < ms)
  {
    esp_task_wdt_reset();
    delay(20);
  }

  if(j->result < 0)
  {
    Serial.printf("[BLE] %s: no answer from the rod in %lu s - dropping the link\n", what, (unsigned long)(ms / 1000));
    jobTimedOut = true;

    if(client != nullptr)
      client->disconnect();

    t0 = millis();

    while(j->result < 0 && millis() - t0 < 5000)
    {
      esp_task_wdt_reset();
      delay(20);
    }

    if(j->result < 0)
    {
      Serial.println("[BLE] Bluetooth stuck - restarting the watch");
      delay(100);
      ESP.restart();
    }
  }

  bool ok = j->result == 1;
  delete j;
  return ok;
}

static void connectToRodAt(BLEAddress addr)
{
  setPhase(BLE_LINK_CONNECTING);
  lostFlag = false;

  client = BLEDevice::createClient();
  client->setClientCallbacks(new ClientCallbacks());

  Serial.printf("[BLE] rod %s, connecting...\n", addr.toString().c_str());
  setNote(pairState == PAIR_WORKING ? "pairing - checking the PIN..." : "rod found - connecting...");

  esp_task_wdt_reset();
  bool ok = client->connect(addr, 0xFF, 10000);   // 10 s at most (the library's own: 30 s)
  esp_task_wdt_reset();

  if(!ok)
  {
    Serial.println("[BLE] could not connect");
    lost();
    setNote("rod found but could not connect");
    return;
  }

  // encrypted link: the PIN the first time, then the saved pairing
  authDone = false;
  authOk = false;
  BLESecurity::resetSecurity();

  BLEClient *c = client;
  bool secured = runWithDeadline([c]() { return c->secureConnection(); }, 15000, "encryption");

  uint32_t waitAuth = millis();

  while(!authDone && client->isConnected() && millis() - waitAuth < 3000)
  {
    esp_task_wdt_reset();
    delay(20);
  }

  if(secured && !authDone)
    authOk = true;                      // (the library said yes; its callback did not come)

  if(!authOk && jobTimedOut)
  {
    // the rod did not answer (busy, too far): try again, NOT a refused PIN
    lost();

    if(pairState == PAIR_WORKING)
      pairState = PAIR_FAILED;          // the pairing screen: TRY AGAIN

    setNote("rod not answering - trying again");
    return;
  }

  if(!authOk)
  {
    Serial.println("[BLE] the rod refused the pairing (wrong PIN, pairing closed, or it forgot this watch)");
    lost();

    if(pairState == PAIR_WORKING)
    {
      pairState = PAIR_FAILED;
    }
    else if(++authFails >= 3)
    {
      // our rod forgot us (Forget my devices in the app): pair again
      Serial.println("[BLE] our rod does not know this watch any more - pair again");
      ble_link_forget();
      pair_restart();                    // into the PAIR YOUR ROD screen
    }

    setNote("rod refused - pair again?");
    return;
  }

  authFails = 0;

  BLERemoteService *service = nullptr;
  runWithDeadline([c, &service]() { service = c->getService(SERVICE_UUID); return service != nullptr; }, 10000, "finding the rod's service");
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
  askLinkTiming(true);

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

  // our rod's LoRa key, if we don't have it yet (a rod without LoRa answers 404)
  uint16_t tag = pairedId.length() > 4 ? (uint16_t)strtol(pairedId.c_str() + 4, nullptr, 16) : 0;

  if(tag != 0 && !lora_link_has_key(tag))
  {
    keyId = newId();
    send(keyId, "/lorakey");
  }

  if(pairState == PAIR_WORKING)
  {
    savePairing(pairWantAddr, String(pairWantId));
    pairState = PAIR_OK;
    Serial.printf("[BLE] paired with %s\n", pairWantId);
  }

  setPhase(BLE_LINK_CONNECTED);
  connectedAt = millis();
  firstAnswerLogged = false;
  setNote("connected - waiting for data");
  Serial.printf("[BLE] connected to the rod (MTU %d)\n", mtu);
}

static void connectToRod()
{
  BLEAddress addr = found->getAddress();
  delete found;
  found = nullptr;
  connectToRodAt(addr);
}

// ---------- public ----------

bool ble_link_is_paired(void)
{
  if(pairedAddr.length() == 0)
    loadPairing();

  return pairedAddr.length() > 0;
}

bool ble_link_rod_id(char *out, int size)
{
  if(!ble_link_is_paired())
    return false;

  strncpy(out, pairedId.c_str(), size - 1);
  out[size - 1] = 0;
  return true;
}

int ble_link_pair_list(pair_rod_t *out, int max)
{
  int n = 0;
  uint32_t now = millis();

  portENTER_CRITICAL(&mux);

  for(int i = 0; i < pairSeenN && n < max; i++)
  {
    if(now - pairSeen[i].at > 15000)
      continue;                        // not seen lately (switched off, or pairing closed)

    memcpy(out[n].id, pairSeen[i].id, sizeof(out[n].id));
    out[n].rssi = pairSeen[i].rssi;
    n++;
  }

  portEXIT_CRITICAL(&mux);
  return n;
}

void ble_link_pair_start(const char *rod_id, uint32_t pin)
{
  strncpy(pairWantId, rod_id, sizeof(pairWantId) - 1);
  pairWantId[sizeof(pairWantId) - 1] = 0;
  pairPin = pin;
  pairState = PAIR_WORKING;
  pairWanted = true;
}

pair_state_t ble_link_pair_state(void)
{
  return pairState;
}

void ble_link_forget(void)
{
#if defined(CONFIG_BLUEDROID_ENABLED)
  if(pairedAddr.length() > 0)
  {
    BLEAddress a(pairedAddr);
    esp_ble_remove_bond_device(a.getNative());
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  ble_store_clear();                    // (the watch is paired to one rod only)
#endif

#if WATCH_LORA
  lora_link_forget_key();               // no LoRa until paired again over Bluetooth
#endif
  rememberLoraOnly(false);

  savePairing("", "");
  authFails = 0;
  Serial.println("[BLE] rod forgotten - pair again");
}

void ble_link_begin(void)
{
  memset(&shared, 0, sizeof(shared));
  shared.lmode = -1;
  shared.chm = -1;
  shared.bat = -1;
  shared.batleft = -1;
  strcpy(shared.state, "");

  loadPairing();

  BLEDevice::init("StrikeSense Watch");
  BLEDevice::setMTU(517);

  // encrypted, PIN protected pairing (the PIN is typed on the watch)
  BLEDevice::setSecurityCallbacks(new WatchSecurity());
  BLESecurity::setCapability(ESP_IO_CAP_IN);
  BLESecurity::setAuthenticationMode(true, true, true);

  if(pairedAddr.length() > 0)
    Serial.printf("[BLE] paired with %s (%s)\n", pairedId.c_str(), pairedAddr.c_str());
  else
    Serial.println("[BLE] not paired yet: pick your rod on the watch");

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
    if(link_merge_via() == LINK_LORA && now - scanEndMs < SCAN_PAUSE_LORA_MS)
      return;

    if(loraOnly(60000))
    {
      setNote("LoRa only (chosen in the phone app)");
      return;
    }

    // the user picked a rod and typed its PIN
    if(pairWanted)
    {
      pairWanted = false;
      String addr;

      portENTER_CRITICAL(&mux);
      for(int i = 0; i < pairSeenN; i++)
        if(strcmp(pairSeen[i].id, pairWantId) == 0)
          addr = pairSeen[i].addr;
      portEXIT_CRITICAL(&mux);

      if(addr.length() == 0)
      {
        pairState = PAIR_FAILED;
        return;
      }

      pairWantAddr = addr;
      connectToRodAt(BLEAddress(addr));
      return;
    }

    seenCount = 0;
    BLEDevice::getScan()->start(pairedAddr.length() > 0 ? 3 : 2, false);   // up to 3 s, stops early when found
    BLEDevice::getScan()->clearResults();
    scans++;
    scanEndMs = millis();

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

  if(loraOnly(3000))
  {
    Serial.println("[BLE] LoRa only chosen in the app: leaving Bluetooth");
    lost();
    setNote("LoRa only (chosen in the phone app)");
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

  // every 2 s: "the watch is here over Bluetooth, hearing you this strong"
  // (the phone app shows it; the answer is not needed)
  if(now - lastPingMs >= 2000)
  {
    lastPingMs = now;
    char path[24];
    snprintf(path, sizeof(path), "/wping?r=%d", rssi);
    send(newId(), path);
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

#if WATCH_LORA
  lora_link_status_t lo = lora_link_get_status();
#else
  lora_link_status_t lo;
  memset(&lo, 0, sizeof(lo));
#endif
  link_merge(out, &lo, now, take_peak);     // rod too far for Bluetooth: LoRa fills in
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
#if WATCH_LORA
  if(link_merge_via() == LINK_LORA)
  {
    uint32_t utc = 0;
    int tz = 0;
    uint8_t cmd = link_path_to_cmd(path, &utc, &tz);

    if(cmd == CMD_SETTIME)
      lora_link_send_settime(utc, tz);
    else if(cmd == CMD_LINKMODE || cmd == CMD_CHANMODE)
      lora_link_send_command_arg(cmd, utc);
    else if(cmd != 0)
      lora_link_send_command(cmd);

    Serial.printf("[LoRa] button %s -> command %d\n", path, cmd);
    return;
  }
#endif

  portENTER_CRITICAL(&mux);
  strncpy(commandPath, path, sizeof(commandPath) - 1);
  commandPath[sizeof(commandPath) - 1] = 0;
  commandBusy = true;
  portEXIT_CRITICAL(&mux);
}

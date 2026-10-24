# Rod: tell the phone how the watch is connected (Bluetooth / LoRa / none),
# and keep a list of the last 10 strikes (type, strongest pull, fight time)
# so the phone can save them, also after "Watch only".
#
#   python3 sim/add_strikelist_watchlink.py      (both rod sketches)

import sys

FILES = ["StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino", "StrikeSense/StrikeSense.ino"]

BLOCK = r'''// ---------- The watch's link, for the phone ----------

unsigned long watchBtAt = 0;            // last "/wping" or "/watch" from the watch over Bluetooth
int watchBtRssi = 0;                    // how strong the watch hears the rod (dBm, 0 = unknown)

int rssiBars(int rssi)
{
  if(rssi == 0) return 0;
  if(rssi >= -60) return 4;
  if(rssi >= -70) return 3;
  if(rssi >= -80) return 2;

  return 1;
}

// "wlink": bt / lora / none. Bluetooth wins when both work (the watch uses it).
void addWatchLink(String& json)
{
  bool bt = watchBtAt != 0 && millis() - watchBtAt < 6000;      // it pings every 2 s
  bool lora = false;
  int loraBarsNow = 0;

#if defined(ENABLE_LORA) && ENABLE_LORA
  lora = watchConnected();
  loraBarsNow = lora ? loraWatchBars() : 0;
#endif

  json += ",\"wlink\":\"";
  json += bt ? "bt" : lora ? "lora" : "none";
  json += "\"";

  addField(json, "wbars", (long)(bt ? rssiBars(watchBtRssi) : loraBarsNow));
  addField(json, "wlora", lora);
}

// ---------- The last strikes, for the phone ----------
//
// A fight lasts from the strike until the rod is quiet (tilt back inside the
// pull allowance, no knock) for 10 s, at most 10 minutes or until reeled in.

const int SLIST_N = 10;
const unsigned long FIGHT_QUIET_MS = 10000;
const unsigned long FIGHT_MAX_MS = 600000;

struct StrikeSummary
{
  unsigned long id;
  char type[20];
  unsigned long startMs;
  unsigned long lastActiveMs;
  float peakTilt;                       // strongest pull, degrees
  float peakKnock;                      // biggest knock, x the knock limit
  int knocks;
  bool done;
};

StrikeSummary slist[SLIST_N];
int slistCount = 0;                     // newest is slist[slistCount - 1]
bool fightWasKnock = false;

void fightOnStrike()
{
  if(slistCount > 0 && !slist[slistCount - 1].done)
    slist[slistCount - 1].done = true;      // a new strike ends the old fight

  if(slistCount == SLIST_N)
  {
    for(int i = 1; i < SLIST_N; i++)
      slist[i - 1] = slist[i];

    slistCount--;
  }

  StrikeSummary& s = slist[slistCount++];
  memset(&s, 0, sizeof(s));
  s.id = strikeEventID;
  strncpy(s.type, strikeType.c_str(), sizeof(s.type) - 1);
  s.startMs = millis();
  s.lastActiveMs = s.startMs;
  s.peakTilt = fabs(tiltDeg);
}

// Every reading
void updateFight()
{
  if(slistCount == 0)
    return;

  StrikeSummary& s = slist[slistCount - 1];

  if(s.done)
    return;

  unsigned long now = millis();
  float thr = knockThreshold > 0 ? knockThreshold : 1;
  bool knock = knockEnergy > thr;
  bool pulled = fabs(tiltDeg) > pullAllow;

  if(knock && !fightWasKnock)
    s.knocks++;

  fightWasKnock = knock;

  if(knock || pulled)                 // an alarm nobody answered is not a fight
    s.lastActiveMs = now;

  if(fabs(tiltDeg) > s.peakTilt)
    s.peakTilt = fabs(tiltDeg);

  if(knockEnergy / thr > s.peakKnock)
    s.peakKnock = knockEnergy / thr;

  if(now - s.lastActiveMs > FIGHT_QUIET_MS || now - s.startMs > FIGHT_MAX_MS || rodState == ST_PAUSED)
    s.done = true;
}

// The watch: "I'm here over Bluetooth, hearing you this strong" (every 2 s)
void handleWatchPing()
{
  watchBtAt = millis();
  watchBtRssi = server.arg("r").toInt();
  server.send(200, "text/plain", "OK");
}

void handleStrikes()
{
  String json;
  json.reserve(120 + SLIST_N * 120);

  json += "{\"time\":";
  json += String((unsigned long)nowEpoch());
  json += ",\"list\":[";

  unsigned long now = millis();

  for(int i = 0; i < slistCount; i++)
  {
    StrikeSummary& s = slist[i];
    unsigned long end = s.done ? s.lastActiveMs : now;

    if(i > 0) json += ",";

    json += "{\"id\":";
    json += String(s.id);
    json += ",\"type\":\"";
    json += s.type;
    json += "\"";
    addField(json, "ago", (long)((now - s.startMs) / 1000));
    addField(json, "fight", (long)((end - s.startMs) / 1000));
    addField(json, "peak", s.peakTilt, 2);
    addField(json, "knock", s.peakKnock, 2);
    addField(json, "knocks", (long)s.knocks);
    addField(json, "done", s.done);
    json += "}";
  }

  json += "]}";

  server.send(200, "application/json", json);
}

'''


def patch(path):
    s = open(path).read()

    if "void addWatchLink(" in s:
        print(path, "already done")
        return

    def rep(a, b, n=1):
        nonlocal s
        assert s.count(a) == n, (path, s.count(a), a)
        s = s.replace(a, b)

    rep("void handleWatch()\n", BLOCK + "void handleWatch()\n")

    # 24 pages were all taken: room for more
    rep("  static const int MAX_ROUTES = 24;", "  static const int MAX_ROUTES = 32;")

    # declarations only (the definitions are further down)
    rep("""void startStrikeRecording()
{""", """void fightOnStrike();
void updateFight();
bool watchConnected();
int loraWatchBars();

void startStrikeRecording()
{""")

    # the watch says how strong it hears the rod: /watch?r=-62
    rep("""void handleWatch()
{
""", """void handleWatch()
{
  watchBtAt = millis();
  watchBtRssi = server.arg("r").toInt();
""")

    rep("""  addField(json, "bat", (long)batPercent);
  addField(json, "batv", batVolts, 2);
""", """  addField(json, "bat", (long)batPercent);
  addField(json, "batv", batVolts, 2);

  addWatchLink(json);
""")

    rep("""  startStrikeRecording();
  startWatchRecording();
""", """  startStrikeRecording();
  startWatchRecording();
  fightOnStrike();
""")

    rep("""  updateWatchRecording();
""", """  updateWatchRecording();
  updateFight();
""")

    rep("""  server.on("/strike", handleStrike);
""", """  server.on("/strike", handleStrike);
  server.on("/strikes", handleStrikes);
  server.on("/wping", handleWatchPing);
""")

    if "bool watchConnected()\n{" in s:
        rep("""void addLoraFields(String& json)
{
  bool connected = watchConnected();""", """int loraWatchBars()
{
  return loraBars(watchRssi);
}

void addLoraFields(String& json)
{
  bool connected = watchConnected();""")

    open(path, "w").write(s)
    print(path, "done")


for f in (sys.argv[1:] or FILES):
    patch(f)

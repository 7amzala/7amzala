#!/usr/bin/env python3
"""Rod side of the watch's BATTERY and LAST STRIKE pages.

- Last strike for the watch: 20 s before + 10 s after the strike, 5 points
  a second (knock as % of the knock limit, tilt x 100). Kept until the rod
  restarts (then: no strike yet, count 0). Served at "/laststrike".
- Rod battery time left: learned from how fast the % drops (after 15 min
  and a 2 % drop); "batleft" in "/watch" (minutes, -1 = still measuring).

Run after add_watch_link.py.

    python3 add_watch_pages.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"{path}: expected {count} x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


# ---------- recorder state ----------
rep('''bool lastStrikeReady = false;
''', '''bool lastStrikeReady = false;

// Last strike for the watch: 30 s at 5 points a second (20 s before, 10 s after)
const int WREC_N = 150;
const int WREC_POST = 50;
int16_t wrecK[WREC_N];                 // ring: knock % of the limit
int16_t wrecT[WREC_N];                 // ring: tilt x 100
int wrecHead = 0;
int wrecCount = 0;
float wrecEnergyMax = 0;
int wrecTick = 0;

int16_t wsK[WREC_N];                   // the finished last strike
int16_t wsT[WREC_N];
int wsCount = 0;
bool wsHave = false;
String wsType = "";
unsigned long wsAt = 0;
float wsTiltPeak = 0;
int wsPull = 0;

bool wsRecording = false;              // a strike is being recorded (10 s after)
int wsPost = 0;
String wsNextType = "";
unsigned long wsNextAt = 0;
float wsNextTiltPeak = 0;
int wsNextPull = 0;

// Rod battery: time left from how fast the % drops
int batStartPct = -1;
unsigned long batStartMs = 0;
long batLeftMin = -1;
''')

# ---------- every reading: feed the watch recorder ----------
rep('''// Called every reading, stores every second reading (50 per second)
void updateRecording()
{''', '''// Watch's last strike: called every reading, keeps 5 points a second
void updateWatchRecording()
{
  if(knockEnergy > wrecEnergyMax)
    wrecEnergyMax = knockEnergy;

  if(++wrecTick < 20)                  // 20 readings x 10 ms = 200 ms
    return;

  wrecTick = 0;

  float thr = knockThreshold > 0 ? knockThreshold : 1;
  float k = wrecEnergyMax / thr * 100;
  float t = tiltDeg * 100;
  wrecEnergyMax = 0;

  wrecK[wrecHead] = (int16_t)(k > 300 ? 300 : k);
  wrecT[wrecHead] = (int16_t)(t > 300 ? 300 : (t < -300 ? -300 : t));
  wrecHead = (wrecHead + 1) % WREC_N;

  if(wrecCount < WREC_N)
    wrecCount++;

  if(!wsRecording)
    return;

  if(fabs(tiltDeg) > wsNextTiltPeak)
    wsNextTiltPeak = fabs(tiltDeg);

  if(++wsPost < WREC_POST)
    return;

  // 10 s after the strike: keep the last 30 s
  wsCount = wrecCount;
  int start = (wrecHead - wrecCount + WREC_N) % WREC_N;

  for(int i = 0; i < wsCount; i++)
  {
    wsK[i] = wrecK[(start + i) % WREC_N];
    wsT[i] = wrecT[(start + i) % WREC_N];
  }

  wsType = wsNextType;
  wsAt = wsNextAt;
  wsTiltPeak = wsNextTiltPeak;
  wsPull = wsNextPull;
  wsHave = true;
  wsRecording = false;
}

void startWatchRecording()
{
  wsRecording = true;
  wsPost = 0;
  wsNextType = strikeType;
  wsNextAt = millis();
  wsNextTiltPeak = fabs(tiltDeg);
  wsNextPull = (int)(pullBucket / bucketSize * 100.0);
}

// Called every reading, stores every second reading (50 per second)
void updateRecording()
{''')

rep('''  startStrikeRecording();''', '''  startStrikeRecording();
  startWatchRecording();''')

rep('''  updateRecording();''', '''  updateRecording();
  updateWatchRecording();''')

# ---------- battery time left ----------
rep('''  batVolts = batVolts == 0 ? v : batVolts * 0.85 + v * 0.15;
  batPercent = lipoPercent(batVolts);
''', '''  batVolts = batVolts == 0 ? v : batVolts * 0.85 + v * 0.15;
  batPercent = lipoPercent(batVolts);

  // time left: from the drop since the start (or since it was charged)
  if(batStartPct < 0 || batPercent > batStartPct + 3)
  {
    batStartPct = batPercent;
    batStartMs = millis();
    batLeftMin = -1;
  }
  else
  {
    float minutes = (millis() - batStartMs) / 60000.0;
    int drop = batStartPct - batPercent;

    if(minutes >= 15 && drop >= 2)
      batLeftMin = (long)(batPercent / (drop / minutes));
  }
''')

rep('''  addField(json, "bat", (long)batPercent);

  json += "}";

  server.send(200, "application/json", json);
}''', '''  addField(json, "bat", (long)batPercent);
  addField(json, "batleft", batLeftMin);

  json += "}";

  server.send(200, "application/json", json);
}

// The watch's LAST STRIKE page (fresh after a rod restart)
void handleLastStrike()
{
  String json;
  json.reserve(WREC_N * 10 + 200);

  json += "{\\"type\\":\\"";
  json += wsType;
  json += "\\"";

  addField(json, "have", wsHave);
  addField(json, "recording", wsRecording);
  addField(json, "ago", wsHave ? (long)((millis() - wsAt) / 1000) : 0L);
  addField(json, "count", (long)strikeEventID);
  addField(json, "tiltpeak", wsTiltPeak, 2);
  addField(json, "pull", (long)wsPull);
  addField(json, "pre", (long)(wsCount - WREC_POST));

  json += ",\\"k\\":[";

  for(int i = 0; i < wsCount; i++)
  {
    if(i > 0) json += ",";
    json += wsK[i];
  }

  json += "],\\"t\\":[";

  for(int i = 0; i < wsCount; i++)
  {
    if(i > 0) json += ",";
    json += wsT[i];
  }

  json += "]}";

  server.send(200, "application/json", json);
}''')

rep('''  server.on("/watch", handleWatch);''', '''  server.on("/watch", handleWatch);
  server.on("/laststrike", handleLastStrike);''')

open(path, 'w').write(s)
print("ok", path)

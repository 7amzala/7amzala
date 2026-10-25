#!/usr/bin/env python3
"""Rod: keep the last 5 strikes with their graphs (/laststrike?n=<number>).

Before, the rod kept only the last strike's graph: when a second strike came
before the phone had fetched the first one's graph, that graph was lost (and
a strike within 10 s of the last one threw away the last one's recording).
Now each finished strike is kept with its own 30 s graph and its zoom
(2 s before + 2 s after at 50 a second), the 5 newest. /laststrike without
n: the newest (as before: the watch's LAST STRIKE page).

    python3 add_strike_keep5.py <sketch.ino>      (edits in place)
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


rep('''int16_t wsK[WREC_N];                   // the finished last strike
int16_t wsT[WREC_N];
int wsCount = 0;
bool wsHave = false;
String wsType = "";
unsigned long wsAt = 0;
float wsTiltPeak = 0;
int wsPull = 0;

''', '')

rep('''int wsNextPull = 0;
''', '''int wsNextPull = 0;
uint32_t wsNextId = 0;                 // the strike's number

// The last 5 finished strikes with their graphs, oldest first. The phone app
// fetches each by its number (/laststrike?n=), so a quick second strike no
// longer loses the first one's graph.
const int ZOOM_EACH = 100;             // 2 s at 50 a second, before and after
const int WS_KEEP = 5;

struct WsRec
{
  uint32_t id;
  int count;                           // points, 200 ms each
  int pre;                             // points before the strike
  int16_t k[WREC_N];
  int16_t t[WREC_N];
  int zpre;                            // zoom: points before the strike
  int zn;
  int16_t zk[ZOOM_EACH * 2];           // zoom: 50 a second around the strike
  int16_t zt[ZOOM_EACH * 2];
  char type[24];
  unsigned long at;
  float tiltPeak;
  int pull;
};

WsRec wsKeep[WS_KEEP];
int wsKeepN = 0;
''')

rep('''  if(++wsPost < WREC_POST)
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
''', '''  if(++wsPost < WREC_POST)
    return;

  finishWatchRecording();              // 10 s after the strike
}

// Keep the strike being recorded with its graph (the last 30 s) and zoom.
// 10 s after it, or sooner when the next strike comes.
void finishWatchRecording()
{
  if(!wsRecording)
    return;

  if(wsKeepN == WS_KEEP)
  {
    for(int i = 1; i < WS_KEEP; i++)
      wsKeep[i - 1] = wsKeep[i];

    wsKeepN--;
  }

  WsRec& r = wsKeep[wsKeepN++];
  int start = (wrecHead - wrecCount + WREC_N) % WREC_N;

  r.id = wsNextId;
  r.count = wrecCount;
  r.pre = wrecCount > wsPost ? wrecCount - wsPost : 0;

  for(int i = 0; i < r.count; i++)
  {
    r.k[i] = wrecK[(start + i) % WREC_N];
    r.t[i] = wrecT[(start + i) % WREC_N];
  }

  // zoom: the dashboard's recording of the same strike (50 a second)
  int zs = savedPre - ZOOM_EACH;
  int ze = savedPre + ZOOM_EACH;

  if(zs < 0) zs = 0;
  if(ze > savedCount) ze = savedCount;
  if(ze < zs) ze = zs;

  float thr = savedThreshold > 0 ? savedThreshold : 1;
  r.zpre = savedPre - zs;
  r.zn = ze - zs;

  for(int i = 0; i < r.zn; i++)
  {
    float k = savedEnergy[zs + i] / thr * 100;
    float t = savedTilt[zs + i] * 100;
    r.zk[i] = (int16_t)(k > 300 ? 300 : k);
    r.zt[i] = (int16_t)(t > 300 ? 300 : (t < -300 ? -300 : t));
  }

  strncpy(r.type, wsNextType.c_str(), sizeof(r.type) - 1);
  r.type[sizeof(r.type) - 1] = 0;
  r.at = wsNextAt;
  r.tiltPeak = wsNextTiltPeak;
  r.pull = wsNextPull;

  wsRecording = false;
}
''')

rep('''void fightOnStrike();
void updateFight();''', '''void fightOnStrike();
void updateFight();
void finishWatchRecording();''')

rep('''  wsRecording = true;
  wsPost = 0;
  wsNextType = strikeType;''', '''  wsRecording = true;
  wsPost = 0;
  wsNextId = strikeEventID;
  wsNextType = strikeType;''')

# a new strike: keep the one still recording first (its zoom is about to be overwritten)
rep('''  sharpPending = false;

  startStrikeRecording();
  startWatchRecording();''', '''  sharpPending = false;

  finishWatchRecording();
  startStrikeRecording();
  startWatchRecording();''')

a = s.index('// The watch\'s LAST STRIKE page (fresh after a rod restart).')
b = s.index('void handleData()')
s = s[:a] + '''// The watch's LAST STRIKE page and the phone app's strike graphs.
// /laststrike: the newest strike; while it is being recorded (the 10 s
// after it) what is there so far, so the watch shows it at once.
// /laststrike?n=12: strike number 12 if the rod still has it (the last 5).
// "zk"/"zt": 2 s before + 2 s after at 50 a second.
void handleLastStrike()
{
  String want = server.arg("n");
  long n = want.length() > 0 ? want.toInt() : -1;

  bool live = wsRecording && (n < 0 || n == (long)wsNextId);
  WsRec* r = nullptr;

  if(!live)
  {
    for(int i = wsKeepN - 1; i >= 0 && r == nullptr; i--)
      if(n < 0 || wsKeep[i].id == (uint32_t)n)
        r = &wsKeep[i];
  }

  bool have = live || r != nullptr;
  int count = live ? wrecCount : (r != nullptr ? r->count : 0);
  int start = (wrecHead - wrecCount + WREC_N) % WREC_N;

  String json;
  json.reserve(WREC_N * 10 + ZOOM_EACH * 20 + 300);

  json += "{\\"type\\":\\"";
  json += live ? wsNextType.c_str() : (r != nullptr ? r->type : "");
  json += "\\"";

  addField(json, "have", have);
  addField(json, "recording", live);
  addField(json, "id", (long)(live ? wsNextId : (r != nullptr ? r->id : 0)));
  addField(json, "ago", have ? (long)((millis() - (live ? wsNextAt : r->at)) / 1000) : 0L);
  addField(json, "count", (long)strikeEventID);
  addField(json, "tiltpeak", live ? wsNextTiltPeak : (r != nullptr ? r->tiltPeak : 0), 2);
  addField(json, "pull", (long)(live ? wsNextPull : (r != nullptr ? r->pull : 0)));
  addField(json, "pre", (long)(live ? count - wsPost : (r != nullptr ? r->pre : 0)));

  json += ",\\"k\\":[";

  for(int i = 0; i < count; i++)
  {
    if(i > 0) json += ",";
    json += String((int)(live ? wrecK[(start + i) % WREC_N] : r->k[i]));
  }

  json += "],\\"t\\":[";

  for(int i = 0; i < count; i++)
  {
    if(i > 0) json += ",";
    json += String((int)(live ? wrecT[(start + i) % WREC_N] : r->t[i]));
  }

  // zoom: while recording from the dashboard's recording, else the kept copy
  int zs = 0, zn = 0, zpre = 0;

  if(live)
  {
    zs = savedPre - ZOOM_EACH;
    int ze = savedPre + ZOOM_EACH;

    if(zs < 0) zs = 0;
    if(ze > savedCount) ze = savedCount;

    zn = ze > zs ? ze - zs : 0;
    zpre = savedPre - zs;
  }
  else if(r != nullptr)
  {
    zn = r->zn;
    zpre = r->zpre;
  }

  float thr = savedThreshold > 0 ? savedThreshold : 1;

  json += "],\\"zpre\\":";
  json += String(zpre);
  json += ",\\"zk\\":[";

  for(int i = 0; i < zn; i++)
  {
    float k = live ? savedEnergy[zs + i] / thr * 100 : r->zk[i];
    if(i > 0) json += ",";
    json += String((int)(k > 300 ? 300 : k));
  }

  json += "],\\"zt\\":[";

  for(int i = 0; i < zn; i++)
  {
    float t = live ? savedTilt[zs + i] * 100 : r->zt[i];
    if(i > 0) json += ",";
    json += String((int)(t > 300 ? 300 : (t < -300 ? -300 : t)));
  }

  json += "]}";

  server.send(200, "application/json", json);
}

''' + s[b:]

open(path, 'w').write(s)
print(f"{path}: last 5 strikes kept with their graphs")

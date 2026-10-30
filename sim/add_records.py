#!/usr/bin/env python3
"""Rod: strike and fight records, kept apart (to make the alarm better).

STRIKE record: the raw sensor (x, y, z), 100 times a second, 5 s before
and 15 s after the strike, with what the rod knew then (knock limit, pull
allowance, sensor noise, waves, wind, the saved pull direction and rest
position, the settings). The simulator can replay it through the real
detection (sim/replay.cpp): every real strike becomes a test case.

FIGHT record: knock and tilt, 10 times a second, from the strike to FISH
OUT / LOST IT / NO FISH (at most 10 min), with the REEL IN moment. Only a
fight with an ending is kept.

Both carry the strike's number in the strike log (the same strike). They
are written to flash when the rod is calm, and the app collects them:
/logdetail?seq=N, /logfight?seq=N. The rod keeps the last 25 strike
records and the last 10 fights (the strike log keeps ~150 strikes now
instead of ~270, to make room).

    python3 add_records.py <sketch.ino>      (edits in place)
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


# ---------- recorders (RAM), next to the watch's strike graphs ----------
rep('''WsRec wsKeep[WS_KEEP];
int wsKeepN = 0;
''', '''WsRec wsKeep[WS_KEEP];
int wsKeepN = 0;

// STRIKE record: raw sensor x, y, z (g x 10000), 100 a second
const int DET_PRE = 500;               // 5 s before the strike
const int DET_N = 2000;                // + 15 s after
int16_t detRing[DET_PRE][3];           // always running: the last 5 s
int detRingHead = 0;
int detRingCount = 0;
int16_t detBuf[DET_N][3];
int detCount = 0;
int detPre = 0;
bool detRecording = false;
bool detReady = false;                 // finished, waiting to be written
uint32_t detSeq = 0;
char detType[20];
float detThr = 0, detAllow = 0, detFloor = 0;
int detWave = 0, detWind = 0;

// FIGHT record: knock (% of the limit) and tilt (x 10 deg), 10 a second
const int FIGHT_N = 6000;              // at most 10 min
int16_t fightK[FIGHT_N];
int16_t fightT[FIGHT_N];
int fightN = 0;
int fightReel = -1;                    // the point where REEL IN was tapped
int fightResult = 0;                   // 1 fish out, 2 lost it, 3 no fish
bool fightRec = false;
bool fightReady = false;               // ended, waiting to be written
uint32_t fightSeq = 0;
unsigned long fightStartMs = 0;
float fightEnergyMax = 0;
int fightTick = 0;
''')

# ---------- every reading: feed the recorders ----------
rep('''  updateRecording();
  updateWatchRecording();
  updateFight();
}''', '''  updateRecording();
  updateWatchRecording();
  updateFight();
  recordsTick();
}''')

# forward declarations (startStrike and the landing code use them)
rep('''void fightOnStrike();
''', '''void fightOnStrike();
void recordsOnStrike();
void recordsTick();
void fightOnReelIn();
void fightEnded(int result);
void recordsBegin();
''')

rep('''  fightOnStrike();

#if defined(ESP32)
  server.pushLive();                    // phone + watch hear it now, not at the next tick
#endif''', '''  fightOnStrike();
  recordsOnStrike();

#if defined(ESP32)
  server.pushLive();                    // phone + watch hear it now, not at the next tick
#endif''')

# REEL IN / the ending mark the fight record
rep('''  landState = 1;
  landStartMs = millis();
  landEndMs = 0;
  landStrikeId = s.id;
  landNews();''', '''  landState = 1;
  landStartMs = millis();
  landEndMs = 0;
  landStrikeId = s.id;
  fightOnReelIn();
  landNews();''')

rep('''  int16_t sec = (int16_t)landSeconds();
  landNews();
''', '''  int16_t sec = (int16_t)landSeconds();
  fightEnded(result);
  landNews();
''')

# ---------- flash: room for the records ----------
rep('''  logSlots = (int)(LittleFS.totalBytes() * 7 / 10 / 4096);   // a strike takes one 4 KB block''',
    '''  logSlots = (int)(LittleFS.totalBytes() * 4 / 10 / 4096);   // a strike takes one 4 KB block (the rest:
                                                              // strike + fight records, see STRIKE + FIGHT RECORDS)''')

rep('''// From loop(): the oldest finished strike not saved yet, one a second, never
// while a strike is being recorded
void logTick()
{
  if(!logReady || wsRecording || millis() - logLastWrite < 1000)
    return;
''', '''void recordsWrite();

// From loop(): the oldest finished strike not saved yet, one a second, never
// while a strike is being recorded
void logTick()
{
  if(!logReady || wsRecording || millis() - logLastWrite < 1000)
    return;

  recordsWrite();                       // a finished strike / fight record first (calm moment)
''')

# /log: say which records the rod has
rep('''      addField(json, "graph", r.count > 1);
      addField(json, "land", (long)r.land);''', '''      addField(json, "graph", r.count > 1);
      addField(json, "detail", recordHas("/sd/", r.seq, detSlots));
      addField(json, "fightrec", recordHas("/fg/", r.seq, fightSlots));
      addField(json, "land", (long)r.land);''')

# ---------- the records: recording, writing, answering ----------
rep('''// /log?after=12: the saved strikes after number 12 (up to 8 at a time)''', '''// =====================================================
// STRIKE + FIGHT RECORDS (see the top of add_records.py)
// =====================================================

const uint32_t DET_MAGIC = 0x53534431;   // "SSD1"
const uint32_t FIGHT_MAGIC = 0x53534631; // "SSF1"
int detSlots = 0;
int fightSlots = 0;

struct DetHead
{
  uint32_t magic;
  uint32_t seq;
  uint16_t hz;
  uint16_t pre;
  uint16_t n;
  uint16_t settingsSize;                // sizeof(UserSettings): a replay only uses them if the same
  char type[20];
  float knockThr, allow, floor;
  int16_t wave, wind;
  uint8_t haveDir, haveRest;
  float dir[3], rest[3];
  UserSettings settings;
};

struct FightHead
{
  uint32_t magic;
  uint32_t seq;
  uint16_t hz;
  uint16_t n;
  int16_t reel;                         // point of REEL IN (-1: none)
  int16_t result;                       // 1 fish out, 2 lost it, 3 no fish
};

static int16_t rec16(float v)
{
  return (int16_t)(v > 32000 ? 32000 : v < -32000 ? -32000 : v);
}

void recordsBegin()
{
  if(!logReady)
    return;

  LittleFS.mkdir("/sd");
  LittleFS.mkdir("/fg");

  // a strike record takes 4 blocks (12 KB), a fight up to 6 (24 KB)
  size_t total = LittleFS.totalBytes();
  detSlots = (int)(total * 25 / 100 / (4 * 4096));
  fightSlots = (int)(total * 16 / 100 / (6 * 4096));
  if(detSlots > 25) detSlots = 25;
  if(fightSlots > 10) fightSlots = 10;

  Serial.printf("RECORDS: keeps the last %d strike records (raw sensor) and %d fights\\n", detSlots, fightSlots);
}

static String recordPath(const char* dir, uint32_t seq, int slots)
{
  return String(dir) + String((unsigned long)(seq % (uint32_t)slots)) + ".bin";
}

// that record is on the rod (its slot holds this strike number)
bool recordHas(const char* dir, uint32_t seq, int slots)
{
  if(!logReady || slots <= 0 || seq == 0)
    return false;

  File f = LittleFS.open(recordPath(dir, seq, slots).c_str(), "r");

  if(!f)
    return false;

  uint32_t head[2] = { 0, 0 };
  bool ok = f.read((uint8_t*)head, 8) == 8 && head[1] == seq;
  f.close();
  return ok;
}

// a strike: keep the last 5 s and record 15 s more; the fight starts
void recordsOnStrike()
{
  uint32_t seq = slistCount > 0 ? slist[slistCount - 1].seq : 0;

  if(detRecording)
  {
    detRecording = false;               // a new strike within 15 s: the old one, shorter
    detReady = true;
  }

  if(!detReady)
  {
    int start = (detRingHead - detRingCount + DET_PRE) % DET_PRE;

    for(int i = 0; i < detRingCount; i++)
      memcpy(detBuf[i], detRing[(start + i) % DET_PRE], sizeof(detBuf[i]));

    detCount = detRingCount;
    detPre = detRingCount;
    detSeq = seq;
    strncpy(detType, strikeType.c_str(), sizeof(detType) - 1);
    detType[sizeof(detType) - 1] = 0;
    detThr = knockThreshold;
    detAllow = pullAllow;
    detFloor = sensorFloor;
    detWave = waveLevel;
    detWind = windLevel;
    detRecording = seq != 0;
  }

  // a new fight record (an old one with no ending is dropped)
  fightRec = seq != 0;
  fightReady = false;
  fightSeq = seq;
  fightN = 0;
  fightReel = -1;
  fightResult = 0;
  fightStartMs = millis();
  fightEnergyMax = 0;
  fightTick = 0;
}

void fightOnReelIn()
{
  if(fightRec)
    fightReel = fightN;
}

void fightEnded(int result)
{
  if(!fightRec)
    return;

  fightRec = false;
  fightResult = result;
  fightReady = fightN > 0;
}

// every reading (100 a second)
void recordsTick()
{
  int16_t raw[3] = { rec16(rawAccel.x * 10000), rec16(rawAccel.y * 10000), rec16(rawAccel.z * 10000) };

  if(detRecording)
  {
    memcpy(detBuf[detCount++], raw, sizeof(raw));

    if(detCount >= DET_N)
    {
      detRecording = false;
      detReady = true;
    }
  }
  else
  {
    memcpy(detRing[detRingHead], raw, sizeof(raw));
    detRingHead = (detRingHead + 1) % DET_PRE;

    if(detRingCount < DET_PRE)
      detRingCount++;
  }

  if(!fightRec)
    return;

  if(knockEnergy > fightEnergyMax)
    fightEnergyMax = knockEnergy;

  if(++fightTick < 10)                  // 10 readings = 100 ms
    return;

  fightTick = 0;

  float thr = knockThreshold > 0 ? knockThreshold : 1;
  fightK[fightN] = rec16(fightEnergyMax / thr * 100);
  fightT[fightN] = rec16(tiltDeg * 10);
  fightEnergyMax = 0;

  // no REEL IN within 5 min of the strike, or 10 min long: no fight to keep
  if(++fightN >= FIGHT_N || (fightReel < 0 && millis() - fightStartMs > 300000UL))
    fightRec = false;
}

// From logTick (a calm moment): a finished record to flash
void recordsWrite()
{
  if(detReady && !detRecording && detSlots > 0)
  {
    detReady = false;

    static DetHead h;
    memset(&h, 0, sizeof(h));
    h.magic = DET_MAGIC;
    h.seq = detSeq;
    h.hz = 100;
    h.pre = detPre;
    h.n = detCount;
    h.settingsSize = sizeof(UserSettings);
    memcpy(h.type, detType, sizeof(h.type));
    h.knockThr = detThr;
    h.allow = detAllow;
    h.floor = detFloor;
    h.wave = detWave;
    h.wind = detWind;
    h.haveDir = haveStoredDir;
    h.haveRest = haveStoredRest;
    h.dir[0] = storedDir.x; h.dir[1] = storedDir.y; h.dir[2] = storedDir.z;
    h.rest[0] = storedRest.x; h.rest[1] = storedRest.y; h.rest[2] = storedRest.z;
    h.settings = settings;

    File f = LittleFS.open(recordPath("/sd/", detSeq, detSlots).c_str(), "w");

    if(f)
    {
      f.write((uint8_t*)&h, sizeof(h));
      f.write((uint8_t*)detBuf, detCount * sizeof(detBuf[0]));
      f.close();
      Serial.printf("RECORDS: strike %lu saved (raw sensor, %.1f s)\\n", (unsigned long)detSeq, detCount / 100.0);
    }

    return;
  }

  if(fightReady && fightSlots > 0)
  {
    fightReady = false;

    FightHead h = { FIGHT_MAGIC, fightSeq, 10, (uint16_t)fightN, (int16_t)fightReel, (int16_t)fightResult };
    File f = LittleFS.open(recordPath("/fg/", fightSeq, fightSlots).c_str(), "w");

    if(f)
    {
      f.write((uint8_t*)&h, sizeof(h));
      f.write((uint8_t*)fightK, fightN * sizeof(int16_t));
      f.write((uint8_t*)fightT, fightN * sizeof(int16_t));
      f.close();
      Serial.printf("RECORDS: fight of strike %lu saved (%d:%02d)\\n", (unsigned long)fightSeq, fightN / 600, fightN / 10 % 60);
    }
  }
}

static void addList(String& json, const char* name, const int16_t* v, int n, int step, int at)
{
  json += ",\\"";
  json += name;
  json += "\\":[";

  for(int i = 0; i < n; i++)
  {
    if(i > 0) json += ",";
    json += String((int)v[i * step + at]);
  }

  json += "]";
}

// /logdetail?seq=12: the strike record (raw sensor + what the rod knew)
void handleLogDetail()
{
  uint32_t seq = (uint32_t)server.arg("seq").toInt();
  static DetHead h;

  if(detRecording || detReady)          // (its buffer is busy with a new strike: ask again later)
  {
    server.send(200, "application/json", "{\\"have\\":false,\\"busy\\":true}");
    return;
  }

  File f = recordHas("/sd/", seq, detSlots) ? LittleFS.open(recordPath("/sd/", seq, detSlots).c_str(), "r") : File();

  if(!f || f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || h.magic != DET_MAGIC || h.n > DET_N)
  {
    if(f) f.close();
    server.send(200, "application/json", "{\\"have\\":false}");
    return;
  }

  f.read((uint8_t*)detBuf, h.n * sizeof(detBuf[0]));   // (the recorder is idle: no strike right now)
  f.close();

  String json;
  json.reserve(h.n * 3 * 7 + 600);
  json += "{\\"have\\":true";
  addField(json, "seq", (long)h.seq);
  addField(json, "hz", (long)h.hz);
  addField(json, "pre", (long)h.pre);
  json += ",\\"type\\":\\"";
  json += h.type;
  json += "\\"";
  addField(json, "knockthr", h.knockThr, 4);
  addField(json, "allow", h.allow, 2);
  addField(json, "floor", h.floor, 5);
  addField(json, "wave", (long)h.wave);
  addField(json, "wind", (long)h.wind);
  addField(json, "hasdir", h.haveDir != 0);
  addField(json, "hasrest", h.haveRest != 0);
  json += ",\\"dir\\":[" + String(h.dir[0], 4) + "," + String(h.dir[1], 4) + "," + String(h.dir[2], 4) + "]";
  json += ",\\"rest\\":[" + String(h.rest[0], 4) + "," + String(h.rest[1], 4) + "," + String(h.rest[2], 4) + "]";

  // the settings, by their saved names
  json += ",\\"settings\\":{";

  for(int i = 0; i < SETTING_COUNT; i++)
  {
    if(i > 0) json += ",";
    json += "\\"";
    json += SETTING_INFO[i].key;
    json += "\\":";
    json += String(h.settings.*(SETTING_INFO[i].field), 4);
  }

  json += "}";
  addList(json, "x", &detBuf[0][0], h.n, 3, 0);
  addList(json, "y", &detBuf[0][0], h.n, 3, 1);
  addList(json, "z", &detBuf[0][0], h.n, 3, 2);
  json += "}";

  server.send(200, "application/json", json);
}

// /logfight?seq=12: the fight record (knock + tilt, 10 a second)
void handleLogFight()
{
  uint32_t seq = (uint32_t)server.arg("seq").toInt();
  FightHead h;

  File f = recordHas("/fg/", seq, fightSlots) ? LittleFS.open(recordPath("/fg/", seq, fightSlots).c_str(), "r") : File();

  if(!f || f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || h.magic != FIGHT_MAGIC || h.n > FIGHT_N || fightRec)
  {
    if(f) f.close();
    server.send(200, "application/json", "{\\"have\\":false}");
    return;
  }

  f.read((uint8_t*)fightK, h.n * sizeof(int16_t));
  f.read((uint8_t*)fightT, h.n * sizeof(int16_t));
  f.close();

  String json;
  json.reserve(h.n * 2 * 6 + 200);
  json += "{\\"have\\":true";
  addField(json, "seq", (long)h.seq);
  addField(json, "hz", (long)h.hz);
  addField(json, "reel", (long)h.reel);
  json += ",\\"result\\":\\"";
  json += h.result == 1 ? "fish" : h.result == 2 ? "lost" : h.result == 3 ? "nofish" : "";
  json += "\\"";
  addList(json, "k", fightK, h.n, 1, 0);
  addList(json, "t", fightT, h.n, 1, 0);
  json += "}";

  server.send(200, "application/json", json);
}

// /log?after=12: the saved strikes after number 12 (up to 8 at a time)''')

rep('''  server.on("/logstrike", handleLogStrike);
''', '''  server.on("/logstrike", handleLogStrike);
  server.on("/logdetail", handleLogDetail);
  server.on("/logfight", handleLogFight);
''')

# after the strike log is ready
rep('''  logSeqSaved = logSeq;
  logReady = true;''', '''  logSeqSaved = logSeq;
  logReady = true;
  recordsBegin();''')

open(path, 'w').write(s)

#!/usr/bin/env python3
"""Rod: every strike saved in flash with its graph (also with no phone near).

The rod keeps the newest ~200-300 strikes (a ring: the oldest is replaced,
nothing is deleted or marked). The app collects what it has not got yet
(/log?after=<number>, /logstrike?seq=<number>) and remembers how far it
got. A strike is written once, when its fight is over and the rod is calm.
Survives switching the rod off. Wear: one 4 KB block per strike, spread
over the whole storage: centuries at 100 strikes a day.

Run after add_strike_keep5.py.

    python3 add_strike_log.py <sketch.ino>      (edits in place)
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


rep('#include <Preferences.h>\n', '#include <Preferences.h>\n#include <LittleFS.h>\n')

rep('''  static const int MAX_ROUTES = 32;''', '''  static const int MAX_ROUTES = 40;''')

rep('''struct StrikeSummary
{
  unsigned long id;''', '''// the strike log (flash), see STRIKE LOG further down
bool logReady = false;
uint32_t logSeq = 0;                    // the newest strike number given out (never repeats)
void logSaveOldest();

struct StrikeSummary
{
  unsigned long id;
  uint32_t seq;                         // number in the strike log (0 = no log)
  bool stored;                          // written to the log''')

rep('''  if(slistCount == SLIST_N)
  {
    for(int i = 1; i < SLIST_N; i++)
      slist[i - 1] = slist[i];''', '''  if(slistCount == SLIST_N)
  {
    logSaveOldest();                    // many strikes in a row: save it now, before it drops out

    for(int i = 1; i < SLIST_N; i++)
      slist[i - 1] = slist[i];''')

rep('''  s.id = strikeEventID;
  strncpy(s.type, strikeType.c_str(), sizeof(s.type) - 1);''', '''  s.id = strikeEventID;
  s.seq = logReady ? ++logSeq : 0;
  strncpy(s.type, strikeType.c_str(), sizeof(s.type) - 1);''')

rep('''    json += "{\\"id\\":";
    json += String(s.id);
    json += ",\\"type\\":\\"";''', '''    json += "{\\"id\\":";
    json += String(s.id);
    json += ",\\"seq\\":";
    json += String((unsigned long)s.seq);
    json += ",\\"type\\":\\"";''')

LOG = r'''// =====================================================
// STRIKE LOG (flash): every strike with its graph, also when no phone is
// near, and after the rod was switched off. The newest ~200 are kept (a
// ring: the oldest is replaced). The app collects what it has not got yet
// (/log, /logstrike) and remembers how far it got: nothing on the rod is
// marked or deleted. Each strike is written once, when its fight is over
// and the rod is calm (writing flash pauses the rod for a moment).
// =====================================================

struct LogRec
{
  uint32_t magic;                       // LOG_MAGIC: a real record
  uint32_t seq;
  uint32_t utc;                         // start, seconds since 1970 (0 = the rod did not know the time)
  uint32_t boot;                        // which power-on
  uint32_t startMs;                     // millis() at the start (same power-on: "ago")
  char type[20];
  float peakTilt;                       // strongest pull, degrees
  float peakKnock;                      // biggest knock, x the knock limit
  int16_t knocks;
  int16_t fight;                        // seconds
  int16_t count;                        // graph points, 200 ms each (0 = no graph)
  int16_t pre;
  int16_t zpre;                         // zoom: 50 a second around the strike
  int16_t zn;
  int16_t k[WREC_N];
  int16_t t[WREC_N];
  int16_t zk[ZOOM_EACH * 2];
  int16_t zt[ZOOM_EACH * 2];
};

const uint32_t LOG_MAGIC = 0x53534C31;  // "SSL1"
const int LOG_MAX = 300;
int logSlots = 0;
uint32_t logBoot = 0;
uint32_t logSeqSaved = 0;
unsigned long logLastWrite = 0;

// (in a struct: the Arduino IDE would put plain functions with these
// types at the top of the file, before the types are known)
struct StrikeLog
{
  static String path(uint32_t seq)
  {
    return "/sl/" + String((unsigned long)(seq % logSlots)) + ".bin";
  }

  static bool read(uint32_t seq, LogRec& r)
  {
    File f = LittleFS.open(path(seq).c_str(), "r");

    if(!f)
      return false;

    bool ok = f.read((uint8_t*)&r, sizeof(r)) == sizeof(r) && r.magic == LOG_MAGIC && r.seq == seq;
    f.close();
    return ok;
  }

  // a strike not written yet (its fight goes on, or the rod was busy)
  static bool pending(uint32_t seq)
  {
    for(int i = 0; i < slistCount; i++)
      if(slist[i].seq == seq && !slist[i].stored)
        return true;

    return false;
  }

  static void write(StrikeSummary& s)
  {
    static LogRec r;                    // (1.5 KB: not on the stack)
    memset(&r, 0, sizeof(r));

    unsigned long ep = nowEpoch();
    r.magic = LOG_MAGIC;
    r.seq = s.seq;
    r.utc = ep ? ep - (millis() - s.startMs) / 1000 : 0;
    r.boot = logBoot;
    r.startMs = s.startMs;
    strncpy(r.type, s.type, sizeof(r.type) - 1);
    r.peakTilt = s.peakTilt;
    r.peakKnock = s.peakKnock;
    r.knocks = s.knocks;
    r.fight = (int16_t)((s.lastActiveMs - s.startMs) / 1000);

    // its graph, if the rod still has it (the last 5)
    for(int i = 0; i < wsKeepN; i++)
    {
      WsRec& w = wsKeep[i];

      if(w.id != s.id)
        continue;

      r.count = w.count;
      r.pre = w.pre;
      memcpy(r.k, w.k, sizeof(r.k));
      memcpy(r.t, w.t, sizeof(r.t));
      r.zpre = w.zpre;
      r.zn = w.zn;
      memcpy(r.zk, w.zk, sizeof(r.zk));
      memcpy(r.zt, w.zt, sizeof(r.zt));
    }

    File f = LittleFS.open(path(s.seq).c_str(), "w");

    if(f)
    {
      f.write((uint8_t*)&r, sizeof(r));
      f.close();
    }

    if(s.seq > logSeqSaved)
    {
      Preferences p;
      p.begin("strikesense", false);
      p.putULong("lseq", s.seq);
      p.end();
      logSeqSaved = s.seq;
    }

    Serial.printf("STRIKE LOG: strike %lu saved (%s%s)\n", (unsigned long)s.seq, r.type, r.count > 1 ? ", with graph" : "");
  }
};

void logBegin()
{
  if(!LittleFS.begin(true))             // the first time: prepares the storage
  {
    Serial.println("STRIKE LOG: no flash storage (Tools > Partition Scheme) - strikes kept in memory only");
    return;
  }

  LittleFS.mkdir("/sl");
  logSlots = (int)(LittleFS.totalBytes() * 7 / 10 / 4096);   // a strike takes one 4 KB block

  if(logSlots > LOG_MAX)
    logSlots = LOG_MAX;

  if(logSlots < 10)
  {
    Serial.println("STRIKE LOG: flash storage too small - strikes kept in memory only");
    return;
  }

  Preferences p;
  p.begin("strikesense", false);
  logSeq = p.getULong("lseq", 0);
  logBoot = p.getULong("lboot", 0) + 1;
  p.putULong("lboot", logBoot);
  p.end();

  logSeqSaved = logSeq;
  logReady = true;
  Serial.printf("STRIKE LOG: keeps the last %d strikes, newest number %lu\n", logSlots, (unsigned long)logSeq);
}

// From loop(): the oldest finished strike not saved yet, one a second, never
// while a strike is being recorded
void logTick()
{
  if(!logReady || wsRecording || millis() - logLastWrite < 1000)
    return;

  for(int i = 0; i < slistCount; i++)
  {
    StrikeSummary& s = slist[i];

    if(s.done && !s.stored && s.seq != 0)
    {
      StrikeLog::write(s);
      s.stored = true;
      logLastWrite = millis();
      return;
    }
  }
}

void logSaveOldest()
{
  StrikeSummary& s = slist[0];

  if(logReady && slistCount > 0 && !s.stored && s.seq != 0)
  {
    s.done = true;
    StrikeLog::write(s);
    s.stored = true;
  }
}

// /log?after=12: the saved strikes after number 12 (up to 8 at a time)
void handleLog()
{
  uint32_t after = (uint32_t)server.arg("after").toInt();

  String json;
  json.reserve(1600);

  json += "{\"boot\":";
  json += String((unsigned long)logBoot);
  addField(json, "last", (long)logSeq);
  addField(json, "time", (long)nowEpoch());
  json += ",\"list\":[";

  int n = 0;

  if(logReady)
  {
    static LogRec r;
    uint32_t from = after + 1;

    if(logSeq >= (uint32_t)logSlots && from < logSeq - logSlots + 1)
      from = logSeq - logSlots + 1;     // older ones were replaced

    for(uint32_t q = from; q <= logSeq && n < 8; q++)
    {
      if(!StrikeLog::read(q, r))
      {
        if(StrikeLog::pending(q))
          break;                        // not saved yet: next time
        continue;                       // lost (the rod was switched off before it was saved)
      }

      if(n++ > 0) json += ",";

      json += "{\"seq\":";
      json += String((unsigned long)r.seq);
      json += ",\"type\":\"";
      json += r.type;
      json += "\"";
      addField(json, "utc", (long)r.utc);
      addField(json, "ago", r.boot == logBoot ? (long)((millis() - r.startMs) / 1000) : -1L);
      addField(json, "fight", (long)r.fight);
      addField(json, "peak", r.peakTilt, 2);
      addField(json, "knock", r.peakKnock, 2);
      addField(json, "knocks", (long)r.knocks);
      addField(json, "graph", r.count > 1);
      json += "}";
    }
  }

  json += "]}";

  server.send(200, "application/json", json);
}

// /logstrike?seq=12: that strike's graph (like /laststrike)
void handleLogStrike()
{
  static LogRec r;
  uint32_t seq = (uint32_t)server.arg("seq").toInt();

  if(!logReady || seq == 0 || !StrikeLog::read(seq, r))
  {
    server.send(200, "application/json", "{\"have\":false}");
    return;
  }

  String json;
  json.reserve(WREC_N * 10 + ZOOM_EACH * 20 + 300);

  json += "{\"type\":\"";
  json += r.type;
  json += "\"";
  addField(json, "have", r.count > 1);
  addField(json, "recording", false);
  addField(json, "seq", (long)r.seq);
  addField(json, "pre", (long)r.pre);

  json += ",\"k\":[";

  for(int i = 0; i < r.count; i++)
  {
    if(i > 0) json += ",";
    json += String((int)r.k[i]);
  }

  json += "],\"t\":[";

  for(int i = 0; i < r.count; i++)
  {
    if(i > 0) json += ",";
    json += String((int)r.t[i]);
  }

  json += "],\"zpre\":";
  json += String((int)r.zpre);
  json += ",\"zk\":[";

  for(int i = 0; i < r.zn; i++)
  {
    if(i > 0) json += ",";
    json += String((int)r.zk[i]);
  }

  json += "],\"zt\":[";

  for(int i = 0; i < r.zn; i++)
  {
    if(i > 0) json += ",";
    json += String((int)r.zt[i]);
  }

  json += "]}";

  server.send(200, "application/json", json);
}

void handleWatch()
'''
rep('void handleWatch()\n', LOG)

rep('''  server.on("/strikes", handleStrikes);''', '''  server.on("/strikes", handleStrikes);
  server.on("/log", handleLog);
  server.on("/logstrike", handleLogStrike);''')

rep('''  server.begin();
''', '''  logBegin();
  server.begin();
''')

rep('''  server.handleClient();
''', '''  server.handleClient();

  logTick();
''')

open(path, 'w').write(s)
print(f"{path}: strike log in flash")

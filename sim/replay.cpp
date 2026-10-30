// Replay a real strike through the rod's own detection, on the PC.
//
// The rod keeps a STRIKE record of every strike: the raw sensor (x, y, z)
// 100 times a second, 5 s before and 15 s after, and what it knew then
// (settings, saved pull direction and rest position). The app collects it
// (/logdetail?seq=N). This tool feeds such a record into the real sketch:
// change the detection, replay all real strikes, and see that every real
// fish still gives an alarm (and the false alarms no longer do).
//
//   g++ -O2 -std=c++17 -Imock -o replay replay.cpp
//   ./replay make strike.json   a strike from a simulated fish, saved as a record
//   ./replay make-false f.json  a false alarm (nothing there), saved as a record
//   ./replay strike.json        replay a record: did the rod alarm, when, which type?
//   ./replay strike.json v      ... with the sketch's Serial output
//
// Exit code 0: an alarm within -1 .. +5 s of the recorded strike.

#define SIM_NO_MAIN
#include "scenarios.cpp"
#include <fstream>
#include <sstream>

// ---------- a tiny reader for the record (flat JSON, number lists) ----------

static std::string text;

static bool has(const std::string& key) { return text.find("\"" + key + "\":") != std::string::npos; }

static double num(const std::string& key, double fallback = 0)
{
  size_t p = text.find("\"" + key + "\":");
  return p == std::string::npos ? fallback : atof(text.c_str() + p + key.size() + 3);
}

static bool flag(const std::string& key)
{
  size_t p = text.find("\"" + key + "\":");
  return p != std::string::npos && text.compare(p + key.size() + 3, 4, "true") == 0;
}

static std::string str(const std::string& key)
{
  size_t p = text.find("\"" + key + "\":\"");
  if (p == std::string::npos) return "";
  p += key.size() + 4;
  return text.substr(p, text.find('"', p) - p);
}

static std::vector<double> list(const std::string& key)
{
  std::vector<double> v;
  size_t p = text.find("\"" + key + "\":[");
  if (p == std::string::npos) return v;
  p += key.size() + 4;
  size_t e = text.find(']', p);
  while (p < e) {
    char* end;
    double d = strtod(text.c_str() + p, &end);
    if (end == text.c_str() + p) break;
    v.push_back(d);
    p = end - text.c_str();
    if (text[p] == ',') p++;
  }
  return v;
}

// ---------- make: a simulated fish, its record saved like the rod does ----------

static int makeRecord(const char* out, bool falseAlarm)
{
  Env calm; calm.waveA = 0.15; calm.waveP = 5; calm.windA = 1;
  env = calm;
  Serial.quiet = !verbose;
  setup();
  run(18);                                     // learning
  cmdSkipPull();
  run(8);
  if (falseAlarm)
  {
    run(3);
    startStrike("SHARP STRIKE");               // an alarm with nothing there (a false alarm)
    run(22);
  }
  else
  {
    events.push_back({ (float)(simMillis / 1000.0 + 1), 1, 3.0, 2, 20 });   // a fish keeps the line pulled
    run(25);                                   // the strike, its 15 s, a calm moment to save it
  }
  uint32_t seq = logSeq;
  server.argVal = String(std::to_string(seq).c_str());
  handleLogDetail();
  std::string body = server.lastBody.c_str();
  if (body.find("\"have\":true") == std::string::npos) { printf("no record for strike %u\n", seq); return 1; }
  std::ofstream(out) << body;
  printf("strike record of a simulated fish saved: %s (%zu bytes, strike %u)\n", out, body.size(), seq);
  return 0;
}

// ---------- replay ----------

static std::vector<double> X, Y, Z;

static void feed(int i)                        // one raw reading (10 ms of sensor time)
{
  cleanAX = X[i] / 10000.0; cleanAY = Y[i] / 10000.0; cleanAZ = Z[i] / 10000.0;
  for (int k = 0; k < 10; k++) { simMillis++; loop(); }
}

int main(int argc, char** argv)
{
  if (argc >= 3 && !strcmp(argv[1], "make")) { verbose = argc > 3; return makeRecord(argv[2], false); }
  if (argc >= 3 && !strcmp(argv[1], "make-false")) { verbose = argc > 3; return makeRecord(argv[2], true); }
  if (argc < 2) { printf("use: replay make out.json | replay record.json [v]\n"); return 2; }

  std::ifstream f(argv[1]);
  std::stringstream ss; ss << f.rdbuf(); text = ss.str();
  X = list("x"); Y = list("y"); Z = list("z");
  int pre = (int)num("pre", 500);
  verbose = argc > 2;

  if (!flag("have") || X.size() < 100 || X.size() != Y.size() || X.size() != Z.size()) { printf("%s: not a strike record\n", argv[1]); return 2; }

  // what the rod knew: saved pull direction + rest position, its settings
  Preferences p;
  p.begin("strikesense", false);
  p.putString("build", buildId());              // (not a "new sketch": nothing is wiped)
  std::vector<double> d = list("dir"), r = list("rest");
  if (flag("hasdir") && d.size() == 3) { p.putBool("hasdir", true); p.putFloat("dx", d[0]); p.putFloat("dy", d[1]); p.putFloat("dz", d[2]); }
  if (flag("hasrest") && r.size() == 3) { p.putBool("hasrest", true); p.putFloat("rx", r[0]); p.putFloat("ry", r[1]); p.putFloat("rz", r[2]); }
  for (int i = 0; i < SETTING_COUNT; i++)
    if (has(SETTING_INFO[i].key)) p.putFloat(SETTING_INFO[i].key, num(SETTING_INFO[i].key));
  p.end();

  sensorSigma = 0;                             // the record already has the real sensor noise
  Serial.quiet = !verbose;
  setup();

  // learn from the first second of the record (played forward and back, so
  // it never jumps): before a slow pull the rest of the 5 s may already
  // move. Then what the real rod had learned is put back exactly.
  int quiet = std::min(100, std::max(20, pre / 2));
  auto calmFor = [&](double secs) {
    for (int k = 0; k < secs * 100; k++) { int c = k % (2 * quiet); feed(c < quiet ? c : 2 * quiet - 1 - c); }
  };
  calmFor(25);
  if (rodState == ST_PULLTEST) cmdSkipPull();
  calmFor(10);

  if (rodState != ST_MONITORING) { printf("the rod did not get ready (state %d)\n", (int)rodState); return 1; }

  knockThreshold = num("knockthr", knockThreshold);
  pullAllow = num("allow", pullAllow);
  sensorFloor = num("floor", sensorFloor);
  waveLevel = (int)num("wave", waveLevel);
  windLevel = (int)num("wind", windLevel);
  if (verbose) printf("as the rod had it: knock limit %.4f g, pull allowance %.2f deg, waves %d, wind %d\n", knockThreshold, pullAllow, waveLevel, windLevel);

  unsigned long before = strikeEventID, startMs = simMillis;
  double firstAt = 1e9; std::string firstType;
  int alarms = 0;
  for (size_t i = 0; i < X.size(); i++) {
    feed((int)i);
    if (strikeEventID != before) {
      before = strikeEventID;
      double at = (simMillis - startMs) / 1000.0 - pre / 100.0;
      if (!alarms) { firstAt = at; firstType = strikeType.c_str(); }
      alarms++;
      if (autoAckAt < 0) handleWatching();     // answered, like the angler would
    }
  }

  std::string was = str("type");
  printf("recorded: %s  |  replay: %s", was.c_str(), alarms ? firstType.c_str() : "NO ALARM");
  if (alarms) printf(" at %+.2f s%s", firstAt, alarms > 1 ? " (and more)" : "");
  printf("\n");
  bool ok = alarms > 0 && firstAt > -1 && firstAt < 5;
  return ok ? 0 : 1;
}

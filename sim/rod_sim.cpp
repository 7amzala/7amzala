// Real-rod simulator: the real StrikeSense sketch on a physics model of
// a 4.20 m surf rod (rod_physics.h), with wind, waves, surf and fish.
//
//   g++ -O2 -std=c++17 -Imock -o rod_sim rod_sim.cpp
//   ./rod_sim survey                      signal and noise along the rod
//   ./rod_sim run <calm|moderate|rough> <sensor m from tip> <lis3dh|adxl355> [seed]
//   ./rod_sim trace <cond> <m from tip> <sensor>     time series for a chart

#include "Arduino.h"
#include <random>
#include <cstring>
#include <vector>
#include <string>
unsigned long simMillis = 0;
SerialT Serial;
float simAX = 0, simAY = 0, simAZ = 1;

#ifndef ENABLE_LORA
#define ENABLE_LORA 0
#endif
#include "../StrikeSense/StrikeSense.ino"
WireT Wire;

#include "rod_physics.h"

std::mt19937 rng(1);
std::normal_distribution<double> N01(0, 1);

// ---------------- conditions ----------------
struct Cond {
  const char* name;
  double windMean, gust;   // m/s, gust strength (fraction)
  double waveT, waveP;     // line tension swing from waves (N), period (s)
  double surfRate;         // surf tugs on the line per second
};
Cond conds[3] = {
  { "calm",     2.0, 0.25, 0.10, 6, 0.0 },
  { "moderate", 6.0, 0.30, 0.40, 7, 0.0 },
  { "rough",   11.0, 0.35, 1.00, 9, 0.08 },
};
const double T0 = 3.0;            // N: line tension at rest (grip lead + current)
const double LINE_WIND = 0.008;   // N per (m/s)^2: wind on ~100 m of line
double WIND_DIR = 0.5;            // rad from straight across (env WIND_DIR: 1.4 = blowing along the line)
double BUFFET = 0.06;             // local flutter strength (fraction of the wind force)
double MIDGUST = 0.4;             // short gusts, relative to the slow gusts

// ---------------- fish and hand events (extra line tension) ----------------
struct Ev { double t0; int kind; double a, b, c; };  // 0 knock(N,dur) 1 pull(N,rise,hold) 2 drop(frac,rise,hold) 3 snake(mean N, shake N, s)
std::vector<Ev> evs;

double eventTension(double t)
{
  double d = 0;
  for (auto& e : evs) {
    double dt = t - e.t0; if (dt < 0) continue;
    if (e.kind == 0 && dt < e.b) d += e.a * sin(M_PI * dt / e.b);
    if (e.kind == 1 || e.kind == 2) {
      double v;
      if (dt < e.b) v = dt / e.b; else if (dt < e.b + e.c) v = 1; else if (dt < e.b + e.c + 1) v = 1 - (dt - e.b - e.c); else v = 0;
      d += (e.kind == 1 ? e.a : -e.a * T0) * v;
    }
    // "snake": a hooked fish shaking its head - the line is tugged back
    // and forth 2-3 times a second, in bursts, on top of a pull (a) that
    // builds in 1 s. a < 0 = the fish swims toward the angler (slack).
    if (e.kind == 3 && dt < e.c) {
      double ramp = dt < 1 ? dt : 1;
      double f = 2.5 + 0.6 * sin(0.7 * dt + e.t0);              // 1.9 .. 3.1 Hz, wandering
      static double ph = 0; ph += 2 * M_PI * f * 0.001;          // (one snake at a time)
      double burst = 0.55 + 0.45 * sin(2 * M_PI * dt / 2.3);     // shakes come in bursts
      d += (e.a >= 0 ? e.a : e.a * T0) * ramp + e.b * burst * sin(ph);
    }
  }
  return d;
}

// ---------------- environment ----------------
struct World {
  Cond c; RodModel rod; int node = 0;
  double gSlow = 0, gMid = 0, gSmooth = 0, buf[2][6] = {}, bufS[2][6] = {};
  double sensLP[3] = {0, 0, 1}; bool sensFirst = true;
  double wf[10], wa[10], wp[10];
  double surfT = -10;
  double sensorSigma = 0.0141;
  double clean[3] = {0, 0, 1};
  bool noWaves = false, noWind = false;

  void init(Cond cc, double fromTip, double sigma)
  {
    c = cc; sensorSigma = sigma;
    // air damping grows with wind speed (a rod moving through moving air)
    sensFirst = true; gSlow = gMid = gSmooth = 0; surfT = -10;
    rod.cfg.damping = 0.025 + 0.003 * (noWind ? 0 : c.windMean);
    rod.build(fromTip); node = rod.sensorNode;
    double sum = 0;
    for (int i = 0; i < 10; i++) { wf[i] = (0.75 + 0.07 * i) / c.waveP; wa[i] = exp(-pow((wf[i] * c.waveP - 1) / 0.25, 2)); wp[i] = 2 * M_PI * (rng() / (double)rng.max()); sum += wa[i] * wa[i]; }
    for (int i = 0; i < 10; i++) wa[i] *= c.waveT / sqrt(2 * sum) * 1.4;
    double U = noWind ? 0 : c.windMean;
    rod.staticStart(T0 + LINE_WIND * U * U, U, WIND_DIR);
  }

  double lineT = T0, windU = 0;
  void step(double t)
  {
    const double dt = 0.001;
    // wind: slow gusts (4 s) + shorter gusts (0.8 s) on the whole rod, plus small
    // local flutter that differs along the rod (turbulence has little energy at 4 Hz)
    gSlow += (-gSlow / 4.0) * dt + c.gust * sqrt(2 * dt / 4.0) * N01(rng);
    gMid += (-gMid / 0.8) * dt + MIDGUST * c.gust * sqrt(2 * dt / 0.8) * N01(rng);
    for (int p = 0; p < 2; p++) for (int k = 0; k < 6; k++) buf[p][k] += (-buf[p][k] / 0.08) * dt + BUFFET * sqrt(2 * dt / 0.08) * N01(rng);
    // real gusts change smoothly: one more 0.3 s smoothing on the gust and flutter signals
    gSmooth += (gSlow + gMid - gSmooth) * dt / 0.3;
    for (int p = 0; p < 2; p++) for (int k = 0; k < 6; k++) bufS[p][k] += (buf[p][k] - bufS[p][k]) * dt / 0.03;
    windU = noWind ? 0 : c.windMean * std::max(0.0, 1 + gSmooth);
    double waves = 0;
    if (!noWaves) {
      double mod = 1 + 0.25 * sin(2 * M_PI * t / 47);
      for (int i = 0; i < 10; i++) waves += wa[i] * sin(2 * M_PI * wf[i] * t + wp[i]);
      waves *= mod;
      if (c.surfRate > 0 && (rng() / (double)rng.max()) < c.surfRate * dt) surfT = t;
      double ds = t - surfT; if (ds >= 0 && ds < 0.4) waves += 0.5 * sin(M_PI * ds / 0.4);
    }
    lineT = std::max(0.05, T0 + LINE_WIND * windU * windU + waves + eventTension(t));
    std::vector<double> F[2];
    rod.forces(lineT, windU, WIND_DIR, bufS, F);
    rod.step(F);
    // accelerometer bandwidth (~100 Hz at 400 readings per second)
    double raw[3]; rod.sensor(node, raw);
    const double aLP = dt / (dt + 1 / (2 * M_PI * 100));
    for (int k = 0; k < 3; k++) { sensLP[k] = sensFirst ? raw[k] : sensLP[k] + aLP * (raw[k] - sensLP[k]); clean[k] = sensLP[k]; }
    sensFirst = false;
  }
};
World W;

void simRead()
{
  simAX = W.clean[0] + N01(rng) * W.sensorSigma;
  simAY = W.clean[1] + N01(rng) * W.sensorSigma;
  simAZ = W.clean[2] + N01(rng) * W.sensorSigma;
}

double sigmaFor(const char* s) { return !strcmp(s, "adxl355") ? 0.00141 : 0.0141; }
Cond condFor(const char* s) { for (auto& c : conds) if (!strcmp(c.name, s)) return c; return conds[0]; }

// ---------------- running the sketch ----------------
struct Hit { double t; std::string what; };
std::vector<Hit> hits;
unsigned long lastID = 0, lastKnock = 0;
double autoAckAt = -1;
FILE* traceOut = nullptr;

void run(double seconds)
{
  double end = simMillis / 1000.0 + seconds;
  while (simMillis / 1000.0 < end) {
    simMillis++;
    double t = simMillis / 1000.0;
    W.step(t);
    loop();
    if (strikeEventID != lastID) { lastID = strikeEventID; hits.push_back({t, strikeType.c_str()}); autoAckAt = t + 2.0; }
    if (knockAlertID != lastKnock) { lastKnock = knockAlertID; hits.push_back({t, "beep"}); }
    if (autoAckAt > 0 && t >= autoAckAt) { autoAckAt = -1; handleWatching(); }
    if (traceOut && simMillis % 10 == 0)
      fprintf(traceOut, "%.2f,%.3f,%.3f,%.5f,%.5f,%.3f,%.3f,%d\n", t, W.lineT, tiltDeg, knockEnergy, knockThreshold, pullAllow, W.rod.tipDeflection() * 100, (int)rodState);
  }
}

void waitArmed()
{
  autoAckAt = -1;
  if (strikeActive) handleWatching();
  for (int i = 0; i < 120 && (rodState != ST_MONITORING || strikeActive); i++) run(0.5);
  run(6);
}

std::string window(double from, double to)
{
  std::string s;
  for (auto& h : hits) if (h.t >= from && h.t < to) { if (!s.empty()) s += "+"; s += h.what; }
  return s.empty() ? "-" : s;
}

int countIn(double from, double to, bool beeps)
{
  int n = 0;
  for (auto& h : hits) if (h.t >= from && h.t < to && ((h.what == "beep") == beeps)) n++;
  return n;
}

int doRun(const char* condName, double fromTip, const char* sensor, int seed)
{
  rng.seed(seed);
  Serial.quiet = true;
  W.init(condFor(condName), fromTip, sigmaFor(sensor));
  for (int i = 0; i < 20000; i++) W.step(i * 0.001);   // let the rod settle
  setup();
  run(16);                                             // learning
  // pull test: pull the line by hand toward the water
  double pullN[3] = { 1.5, 2.5, 4.0 };
  int tries = 0;
  for (; tries < 3 && rodState == ST_PULLTEST; tries++) {
    double t = simMillis / 1000.0;
    evs.push_back({ t + 0.5, 1, pullN[tries], 0.5, 1.5 });
    for (int i = 0; i < 16 && rodState == ST_PULLTEST; i++) run(0.5);
  }
  bool pullOk = rodState != ST_PULLTEST;
  if (!pullOk) handleSkipPull();
  waitArmed();

  double t = simMillis / 1000.0;
  run(600);
  int falseStrikes = countIn(t, t + 600, false), falseBeeps = countIn(t, t + 600, true);
  int falseShake = 0;
  for (auto& h : hits) if (h.t >= t && h.t < t + 600 && h.what == "SHAKING FISH") falseShake++;

  struct Test { const char* name; std::vector<Ev> e; };
  std::vector<Test> tests = {
    { "knock_small",  { { 1, 0, 0.25, 0.06, 0 } } },
    { "knock_medium", { { 1, 0, 0.60, 0.06, 0 } } },
    { "repeated",     { { 1, 0, 0.30, 0.06, 0 }, { 1.8, 0, 0.30, 0.06, 0 }, { 2.6, 0, 0.30, 0.06, 0 } } },
    { "slow_pull",    { { 1, 1, 1.00, 3.0, 8 } } },
    { "drop_back",    { { 1, 2, 0.70, 1.5, 8 } } },
    { "sharp",        { { 1, 0, 2.50, 0.08, 0 }, { 1.05, 1, 2.5, 0.3, 4 } } },
    { "snake_shake",  { { 1, 3, 0.00, 0.60, 10 } } },   // shaking on the spot
    { "snake_small",  { { 1, 3, 0.30, 0.60, 10 } } },   // shaking + small pull
    { "snake_big",    { { 1, 3, 0.80, 1.50, 10 } } },   // shaking + real run
    { "snake_slack",  { { 1, 3, -0.40, 0.60, 10 } } },  // shaking toward the angler
    { "shake_02",     { { 1, 3, 0.00, 0.20, 10 } } },   // gentler and gentler shaking, no pull
    { "shake_01",     { { 1, 3, 0.00, 0.10, 10 } } },
    { "shake_005",    { { 1, 3, 0.00, 0.05, 10 } } },
    { "shake_0025",   { { 1, 3, 0.00, 0.025, 10 } } },
    { "knock_tiny",   { { 1, 0, 0.10, 0.06, 0 } } },    // for comparison: a gentle single knock
    { "knock_005",    { { 1, 0, 0.05, 0.06, 0 } } },
  };
  printf("{\"cond\":\"%s\",\"pos\":%.2f,\"sensor\":\"%s\",\"seed\":%d,\"pulltest\":%s,\"tries\":%d,\"need\":%.2f,\"false10min\":%d,\"shakefalse\":%d,\"beeps10min\":%d",
         condName, fromTip, sensor, seed, pullOk ? "true" : "false", tries, pullTestNeed(), falseStrikes, falseShake, falseBeeps);
  for (auto& ts : tests) {
    waitArmed();
    double t0 = simMillis / 1000.0;
    for (auto e : ts.e) { e.t0 += t0; evs.push_back(e); }
    run(14);
    printf(",\"%s\":\"%s\"", ts.name, window(t0 + 0.5, t0 + 14).c_str());
  }
  waitArmed();
  printf(",\"thr\":%.4f,\"allow\":%.2f,\"sea\":%d}\n", knockThreshold, pullAllow, seaState);
  return 0;
}

// ---------------- survey: signal and noise along the rod ----------------
// Same filters as the sketch: gravity = 0.3 s low-pass, vibration = raw - 4 Hz low-pass,
// knock energy = RMS over 80 ms, all on 100 Hz averages of 400 Hz readings.
struct Meter {
  double lpF[3] = {0,0,0}, lpG[3] = {0,0,0}, eSq = 0; bool first = true; double acc[3] = {0,0,0}; int nAcc = 0;
  double energy = 0, tilt0[3] = {0,0,0}; bool haveRest = false;
  double sigma = 0;
  void sample(const double c[3], int ms)
  {
    if (ms % 10 != 0 && ms % 10 != 2 && ms % 10 != 5 && ms % 10 != 7) return;   // 400 Hz
    for (int k = 0; k < 3; k++) acc[k] += c[k] + N01(rng) * sigma;
    if (++nAcc < 4) return;
    double r[3]; for (int k = 0; k < 3; k++) { r[k] = acc[k] / 4; acc[k] = 0; } nAcc = 0;
    const double dt = 0.01, aF = dt / (dt + 1 / (2 * M_PI * 4)), aG = dt / (dt + 0.3), aE = dt / (dt + 0.08);
    if (first) { for (int k = 0; k < 3; k++) lpF[k] = lpG[k] = r[k]; first = false; }
    double v2 = 0;
    for (int k = 0; k < 3; k++) { lpF[k] += aF * (r[k] - lpF[k]); lpG[k] += aG * (r[k] - lpG[k]); double v = r[k] - lpF[k]; v2 += v * v; }
    eSq += aE * (v2 - eSq); energy = sqrt(eSq);
  }
  void setRest() { for (int k = 0; k < 3; k++) tilt0[k] = lpG[k]; haveRest = true; }
  double tiltDeg()
  {
    double a = 0, b = 0, d = 0; for (int k = 0; k < 3; k++) { a += lpG[k] * tilt0[k]; b += lpG[k] * lpG[k]; d += tilt0[k] * tilt0[k]; }
    double c = a / sqrt(b * d); if (c > 1) c = 1; return acos(c) * 180 / M_PI;
  }
};

void survey()
{
  const double pos[] = { 0.05, 0.10, 0.20, 0.30, 0.40, 0.55, 0.70, 0.85, 1.00, 1.20, 1.40 };
  printf("[\n");
  bool firstRow = true;
  for (double p : pos) {
    // 1) knock 0.5 N and pull 1 N, no wind, no waves, no sensor noise
    double knock = 0, pullTilt = 0, sharpTilt = 0;
    {
      rng.seed(5); W.noWaves = W.noWind = true; W.init(conds[0], p, 0); evs.clear();
      Meter m; m.sigma = 0;
      int ms = 0;
      for (; ms < 8000; ms++) { W.step(ms * 0.001); m.sample(W.clean, ms); }
      m.setRest();
      evs.push_back({ 9.0, 0, 0.5, 0.06, 0 });
      for (; ms < 11000; ms++) { W.step(ms * 0.001); m.sample(W.clean, ms); if (ms > 9000) knock = std::max(knock, m.energy); }
      evs.clear(); evs.push_back({ 12.0, 1, 1.0, 1.0, 6 });
      for (; ms < 17000; ms++) { W.step(ms * 0.001); m.sample(W.clean, ms); }
      pullTilt = m.tiltDeg();
      W.noWaves = W.noWind = false; evs.clear();
    }
    // 2) each condition: vibration from wind/waves and tilt swing, no fish
    double windE[3], tiltPP[3], floorE[2];
    for (int ci = 0; ci < 3; ci++) {
      rng.seed(11 + ci); W.init(conds[ci], p, 0); evs.clear();
      Meter m; m.sigma = 0;
      int ms = 0;
      for (; ms < 20000; ms++) { W.step(ms * 0.001); m.sample(W.clean, ms); }
      // rest = average gravity over a while
      double sumE = 0; int nE = 0; double tmin = 1e9, tmax = -1e9;
      double avg[3] = {0,0,0}; int na = 0;
      std::vector<double> gs;
      for (; ms < 140000; ms++) {
        W.step(ms * 0.001); m.sample(W.clean, ms);
        if (ms % 10 == 0) { sumE += m.energy; nE++; for (int k = 0; k < 3; k++) avg[k] += m.lpG[k]; na++; }
      }
      for (int k = 0; k < 3; k++) m.tilt0[k] = avg[k] / na;
      for (; ms < 260000; ms++) {
        W.step(ms * 0.001); m.sample(W.clean, ms);
        if (ms % 10 == 0) {
          // signed tilt in the line's plane: angle of gravity in x-z
          double a1 = atan2(m.lpG[2], m.lpG[0]), a0 = atan2(m.tilt0[2], m.tilt0[0]);
          double d = (a1 - a0) * 180 / M_PI; tmin = std::min(tmin, d); tmax = std::max(tmax, d);
        }
      }
      windE[ci] = sumE / nE; tiltPP[ci] = tmax - tmin;
    }
    // 3) sensor noise floor (rod still), both sensors
    for (int si = 0; si < 2; si++) {
      rng.seed(21); W.noWaves = W.noWind = true; W.init(conds[0], p, si ? 0.00141 : 0.0141);
      Meter m; m.sigma = W.sensorSigma; double s = 0; int n = 0;
      for (int ms = 0; ms < 20000; ms++) { W.step(ms * 0.001); m.sample(W.clean, ms); if (ms > 5000 && ms % 10 == 0) { s += m.energy; n++; } }
      floorE[si] = s / n; W.noWaves = W.noWind = false;
    }
    RodModel tmp; tmp.build(p);
    printf("%s  {\"pos\":%.2f,\"od_mm\":%.1f,\"knock05\":%.5f,\"pull1N\":%.3f,\"windE\":[%.5f,%.5f,%.5f],\"tiltPP\":[%.3f,%.3f,%.3f],\"floorLIS\":%.5f,\"floorADXL\":%.5f}\n",
           firstRow ? "" : ",", p, tmp.od[tmp.sensorNode] * 1000, knock, pullTilt, windE[0], windE[1], windE[2], tiltPP[0], tiltPP[1], tiltPP[2], floorE[0], floorE[1]);
    firstRow = false;
    fflush(stdout);
  }
  printf("]\n");
}

// shaking level along the line / sideways, wind only, then gentle fish shakes
void shakeTrace(const char* condName, double fromTip, const char* sensor, int seed)
{
  rng.seed(seed);
  Serial.quiet = true;
  W.init(condFor(condName), fromTip, sigmaFor(sensor));
  for (int i = 0; i < 20000; i++) W.step(i * 0.001);
  setup();
  run(16);
  if (rodState == ST_PULLTEST) { double t = simMillis / 1000.0; evs.push_back({ t + 0.5, 1, 2.5, 0.5, 1.5 }); for (int i = 0; i < 16 && rodState == ST_PULLTEST; i++) run(0.5); }
  if (rodState == ST_PULLTEST) handleSkipPull();
  waitArmed();
  double peakWind = 0, peakWindRatio = 0, maxBase = 0;
  for (int i = 0; i < 1800; i++) {            // 3 min wind + waves only
    run(0.1);
    if (i > 300) { peakWind = std::max(peakWind, (double)shakeRmsIn); if (shakeRmsSide > 0) peakWindRatio = std::max(peakWindRatio, (double)(shakeRmsIn / shakeRmsSide)); maxBase = std::max(maxBase, (double)shakeBase); }
  }
  printf("%s %s dir=%.1f  wind only: base %.4f g, peak along-line %.4f g (= %.1f x base), peak along/side %.1f\n",
         condName, sensor, WIND_DIR, shakeBase, peakWind, peakWind / std::max(1e-6, (double)shakeBase), peakWindRatio);
  double amps[] = { 0.025, 0.05, 0.1, 0.2 };
  for (double a : amps) {
    waitArmed();
    double t0 = simMillis / 1000.0;
    evs.push_back({ t0 + 1, 3, 0.0, a, 8 });
    double pk = 0, ratio = 0, base = shakeBase; int over = 0;
    for (int i = 0; i < 90; i++) { run(0.1); if (i > 20) { pk = std::max(pk, (double)shakeRmsIn); ratio = std::max(ratio, (double)(shakeRmsIn / std::max(1e-6f, shakeRmsSide))); } }
    printf("   shake %.3f N: peak along-line %.4f g = %.1f x base, along/side %.1f\n", a, pk, pk / std::max(1e-6, base), ratio);
  }
}

int main(int argc, char** argv)
{
  if (getenv("WIND_DIR")) WIND_DIR = atof(getenv("WIND_DIR"));
  if (getenv("SHAKE_K")) SHAKE_K = atof(getenv("SHAKE_K"));
  if (getenv("SHAKE_FLOOR")) SHAKE_FLOOR = atof(getenv("SHAKE_FLOOR"));
  if (getenv("SHAKE_SIDE")) SHAKE_SIDE_RATIO = atof(getenv("SHAKE_SIDE"));
  if (getenv("SHAKE_HOLD")) SHAKE_HOLD_MS = atol(getenv("SHAKE_HOLD"));
  if (argc >= 5 && !strcmp(argv[1], "strace")) { shakeTrace(argv[2], atof(argv[3]), argv[4], argc > 5 ? atoi(argv[5]) : 1); return 0; }
  if (argc >= 2 && !strcmp(argv[1], "survey")) { survey(); return 0; }
  if (argc >= 5 && (!strcmp(argv[1], "run") || !strcmp(argv[1], "trace"))) {
    if (!strcmp(argv[1], "trace")) traceOut = stdout;
    return doRun(argv[2], atof(argv[3]), argv[4], argc > 5 ? atoi(argv[5]) : 1);
  }
  printf("usage: rod_sim survey | run <calm|moderate|rough> <m from tip> <lis3dh|adxl355> [seed]\n");
  return 1;
}

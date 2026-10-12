// Host simulator for StrikeSense.ino
//
// Runs the real sketch on a PC with a simulated rod tip: waves,
// wind, tide and fish (knocks, slow pulls, drop-backs).
//
//   g++ -O2 -std=c++17 -Imock -o scenarios scenarios.cpp && ./scenarios
//   ./scenarios x      also print the sketch's Serial output
//   ./scenarios x v    also print every strike with details

#include "Arduino.h"
#include <random>
#include <cstring>
#include <vector>
unsigned long simMillis = 0;
bool verbose = true;
SerialT Serial;

float simAX = 0, simAY = 0, simAZ = 1;

#include "../StrikeSense/StrikeSense.ino"
WiFiT WiFi; WireT Wire;

// ---------------- environment model ----------------
struct Env {
  float waveA = 0, waveP = 6, windA = 0, tideRate = 0, windVib = 0;
};
Env env;
std::mt19937 rng(42);
std::normal_distribution<float> N(0, 1);

struct Event { float t0; int kind; float a, b, c; }; // kind 0 knock(K), 1 pull(deg,rise,hold), 2 drop(deg,rise,hold)
std::vector<Event> events;

float windState = 0;
float cleanAX = 0, cleanAY = 0, cleanAZ = 1;

void computeSensor(double t)
{
  const float D = 0.0174533f;
  float pitch = 0, roll = 0;
  // waves: main swell + a bit of second harmonic, slowly varying amplitude
  float ampMod = 1.0 + 0.25 * sin(2 * M_PI * t / 47.0);
  pitch += env.waveA * ampMod * sin(2 * M_PI * t / env.waveP) + 0.3 * env.waveA * sin(2 * M_PI * t / (env.waveP * 0.53) + 1.0);
  roll += 0.4 * env.waveA * sin(2 * M_PI * t / env.waveP + 0.7);
  // wind: low-passed random sway, mostly sideways
  windState += (N(rng) * 3.0f - windState) * 0.002f;
  roll += env.windA * windState;
  pitch += 0.3f * env.windA * windState;
  static double tideAngle = 0; tideAngle += env.tideRate / 60.0 * 0.001; pitch += tideAngle;
  float vib[3] = {0, 0, 0};
  for (auto& e : events) {
    float dt = t - e.t0;
    if (dt < 0) continue;
    if (e.kind == 0 && dt < 0.35) {
      float s = e.a * exp(-dt * 12) * sin(2 * M_PI * 9 * dt);
      vib[2] += s; vib[0] += 0.4 * s;
      pitch += 0.15 * e.a / 0.05 * exp(-dt * 8) * (dt < 0.35);
    }
    if (e.kind == 1 || e.kind == 2) {
      float sign = e.kind == 1 ? 1 : -1;
      float v;
      if (dt < e.b) v = dt / e.b; else if (dt < e.b + e.c) v = 1; else if (dt < e.b + e.c + 1) v = 1 - (dt - e.b - e.c); else v = 0;
      pitch += sign * e.a * v;
    }
  }
  float th0 = 30 * D, p = pitch * D, r = roll * D;
  // rest gravity in sensor frame
  float gx = -sin(th0), gy = 0, gz = cos(th0);
  // pitch about y
  float x1 = gx * cos(p) + gz * sin(p), z1 = -gx * sin(p) + gz * cos(p), y1 = gy;
  // roll about x
  float y2 = y1 * cos(r) - z1 * sin(r), z2 = y1 * sin(r) + z1 * cos(r);
  float nz = env.windVib * fabs(windState);
  cleanAX = x1 + vib[0] + N(rng) * nz;
  cleanAY = y2 + vib[1] + N(rng) * nz;
  cleanAZ = z2 + vib[2] + N(rng) * nz;
}

// Sensor's own noise, fresh for every reading. Calibrated so the old
// sketch settings (200 Hz, no averaging) give the 0.017 g noise seen
// on the real LIS3DH lying still. At 400 Hz the noise per reading is
// about 1.4x higher (wider bandwidth).
float sensorSigma = 0.0141;
void simRead()
{
  simAX = cleanAX + N(rng) * sensorSigma;
  simAY = cleanAY + N(rng) * sensorSigma;
  simAZ = cleanAZ + N(rng) * sensorSigma;
}

struct Strike { double t; std::string type; };
std::vector<Strike> strikes;
unsigned long lastID = 0, lastKnock = 0; int knockAlerts = 0;
double autoAckAt = -1;

void run(double seconds, bool autoAck = true)
{
  double end = simMillis / 1000.0 + seconds;
  while (simMillis / 1000.0 < end) {
    simMillis++;
    computeSensor(simMillis / 1000.0);
    loop();
    if (strikeEventID != lastID) {
      lastID = strikeEventID;
      strikes.push_back({simMillis / 1000.0, strikeType.c_str()});
      if(verbose) printf("    ** STRIKE %s at %.2fs tilt=%.2f allow=%.2f bucket=%.2f/%.2f\n", strikeType.c_str(), simMillis / 1000.0, tiltDeg, pullAllow, pullBucket, bucketSize);
      if (autoAck) autoAckAt = simMillis / 1000.0 + 2.0;
    }
    if (knockAlertID != lastKnock) { lastKnock = knockAlertID; knockAlerts++; }
    if (autoAckAt > 0 && simMillis / 1000.0 >= autoAckAt) { autoAckAt = -1; handleWatching(); }
  }
}


void pullTest(double t) { events.push_back({(float)t, 1, pullTestNeed() * 1.3f, 0.8, 1.5}); }
Vec3 modelG(float pitchDeg) { const float D = 0.0174533f; float th0 = 30 * D, p = pitchDeg * D; float gx = -sin(th0), gz = cos(th0); return vec(gx * cos(p) + gz * sin(p), 0, -gx * sin(p) + gz * cos(p)); }
// 1.00 = learned pull direction matches the real one
float truthDot() { Vec3 d = vNorm(vPerp(vSub(modelG(1), modelG(0)), restDir)); return vDot(d, pullDir); }

void waitArmed() { autoAckAt = -1; if (strikeActive) handleWatching(); for (int i = 0; i < 120 && (rodState != ST_MONITORING || strikeActive); i++) run(0.5); run(20); }
int phase(const char* name, double secs)
{
  size_t before = strikes.size(); int kb = knockAlerts;
  run(secs);
  int n = strikes.size() - before;
  std::string types; for (size_t i = before; i < strikes.size(); i++) { char b[64]; snprintf(b,64,"%s@+%.1fs ", strikes[i].type.c_str(), strikes[i].t - (simMillis/1000.0 - secs)); types += b; }
  printf("  %-40s strikes=%d [%s] knocks=%d | thr=%.4f swing=%.2f per=%.1f wind=%.2f allow=%.2f bucket=%.1f fill=%.2f sea=%d dir=%d\n", name, n, types.size() > 120 ? "many" : types.c_str(), knockAlerts - kb,
         knockThreshold, waveSwing, wavePeriod, windSwing, pullAllow, bucketSize, waveFill, seaState, haveDir);
  return n;
}

void scenario(const char* title, Env e, bool doPullTest, float pullDeg, float falseMinutes)
{
  printf("\n=== %s ===\n", title);
  env = e; events.clear(); strikes.clear();
  startEnvironmentLearning();
  run(15.5);
  if (doPullTest) { pullTest(simMillis / 1000.0 + 1); for (int i = 0; i < 30 && rodState == ST_PULLTEST; i++) run(0.5); } else handleSkipPull();
  printf("  after pull test: %s dir=%d dirdot=%.2f need=%.1f\n", stateName(), haveDir, truthDot(), pullTestNeed());
  if (!strcmp(stateName(), "pulltest")) handleSkipPull();
  char buf[80]; snprintf(buf, 80, "%.0f min no fish (false alarms?)", falseMinutes);
  phase(buf, falseMinutes * 60);
  waitArmed(); double t = simMillis / 1000.0;
  events.push_back({(float)t + 1, 0, 0.06, 0, 0});
  phase("single knock 0.06g", 8);
  waitArmed(); t = simMillis / 1000.0;
  for (int i = 0; i < 3; i++) events.push_back({(float)(t + 1 + i * 0.9), 0, 0.06, 0, 0});
  phase("3 knocks 0.06g in 2s", 12);
  waitArmed(); t = simMillis / 1000.0;
  events.push_back({(float)t + 1, 0, 0.30, 0, 0});
  phase("one big knock 0.30g", 12);
  waitArmed(); t = simMillis / 1000.0;
  events.push_back({(float)t + 1, 0, 0.20, 0, 0});
  events.push_back({(float)t + 1, 1, 3.0f + e.waveA, 0.25, 1.5});
  phase("sharp: knock 0.2g + fast 3+wave deg pull", 12);
  waitArmed(); t = simMillis / 1000.0;
  events.push_back({(float)t + 1, 1, pullDeg, 4, 6});
  phase("slow pull", 15);
  waitArmed(); t = simMillis / 1000.0;
  events.push_back({(float)t + 1, 2, pullDeg, 3, 6});
  phase("drop-back", 15);
  phase("5 min after (false alarms?)", 300);
}

#ifndef SIM_NO_MAIN
int main(int argc, char** argv)
{
  Serial.quiet = argc < 2; verbose = argc > 2;
  setup();
  Env calm; calm.waveA = 0.15; calm.waveP = 5; calm.windA = 1;
  scenario("CALM, pull 2 deg", calm, true, 2.0, 20);
  Env mod; mod.waveA = 1.0; mod.waveP = 6; mod.windA = 5; mod.windVib = 0.01; mod.tideRate = 0.5;
  scenario("MODERATE, pull 3 deg", mod, true, 3, 20);
  Env rough; rough.waveA = 2.5; rough.waveP = 8; rough.windA = 12; rough.windVib = 0.02; rough.tideRate = 1.0;
  scenario("ROUGH, pull 5 deg", rough, true, 5, 20);
  scenario("ROUGH, pull 3.5 deg", rough, true, 3.5, 5);
  Env vrough = rough; vrough.waveA = 4; vrough.waveP = 10;
  scenario("VERY ROUGH 4deg/10s, pull 6 deg", vrough, true, 6, 20);
  Preferences::f().clear(); haveStoredDir = false;
  scenario("MODERATE, no direction, pull 3 deg", mod, false, 3, 20);
}
#endif

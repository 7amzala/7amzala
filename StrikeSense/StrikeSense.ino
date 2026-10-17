#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Preferences.h>
#include <SparkFunLIS3DH.h>

// LoRa link to the watch (XIAO ESP32-S3 + Wio-SX1262).
// Set to 0 to build without LoRa.
#ifndef ENABLE_LORA
#define ENABLE_LORA 1
#endif

#if ENABLE_LORA
#include <SPI.h>
#include <RadioLib.h>
#endif

#include "strikesense_protocol.h"

void loraOnStrike();
void addLoraFields(String& json);

// Sensor: ADXL355 (EVAL-ADXL355-PMDZ, I2C) or LIS3DH, found automatically.
// ADXL355 wiring: pin 1 SCL -> D5, pin 2 SDA -> D4, pin 5 GND, pin 6 3V3,
// on the sensor link pin 3 -> 12 and pin 4 -> 11.
LIS3DH myIMU(I2C_MODE, 0x19);
WebServer server(80);
Preferences prefs;

const char* apName = "StrikeSense-Test";
const char* apPassword = "12345678";

// =====================================================
// TUNING
//
// All the numbers you may want to change are here.
// See TUNING.md for which number fixes which problem.
// =====================================================

// ---------- Sampling ----------

const unsigned long SAMPLE_MS = 10;      // 100 processed readings per second
const float DT = 0.010;                  // seconds per reading
const int SENSOR_RATE = 400;             // Hz, sensor output rate
const unsigned long OVERSAMPLE_US = 2500; // read every 2.5 ms, average ~4 per step (less noise)

// ---------- Signal filters ----------

const float GRAVITY_TAU = 0.30;          // s, rod angle smoothing
const float VIBRATION_TAU = 0.04;        // s, only shaking faster than ~4 Hz counts as vibration
const float ENERGY_TAU = 0.08;           // s, knock energy smoothing (smoother noise, knocks last 200+ ms)

// ---------- Environment ----------

const unsigned long LEARN_TIME = 15000;  // quick start learning (covers 1-2 waves)
const float NOISE_TAU = 30.0;            // s, background vibration tracking
const float GUST_NOISE_TAU = 3.0;        // s, fast vibration level (wind gusts)
const float GUST_NOISE_MUL = 1.0;        // limit rises this much with a gust (1 = same ratio as the gust)
const float WAVE_TAU = 20.0;             // s, wave size tracking
const float WIND_TAU = 20.0;             // s, sideways sway tracking
const float BASELINE_TAU = 60.0;         // s, rest position follows tide
const unsigned long SEA_STATE_HOLD = 30000; // auto sea state must hold this long to change

// ---------- Knock detector ----------

const float KNOCK_FLOOR_LIS3DH = 0.010;  // g, threshold never goes below this (LIS3DH)
const float KNOCK_FLOOR_ADXL355 = 0.006; // g, same for the ~10x quieter ADXL355
float KNOCK_FLOOR = KNOCK_FLOOR_LIS3DH;  // set in setup() for the sensor found
const float KNOCK_CEIL = 0.250;          // g, threshold never goes above this
const float KNOCK_RELEASE = 0.50;        // knock ends halfway between normal noise and the limit
const int KNOCK_CONFIRM = 3;             // readings in a row above threshold (30 ms)
const unsigned long KNOCK_MAX_MS = 800;  // longer than this = wind / handling
const unsigned long KNOCK_GAP_MS = 150;  // dead time after each knock
// Repeated-bite count/window, sharp level, pull angle/time, drop-back
// and knock beep are LIVE SETTINGS (dashboard -> SETTINGS). The
// defaults are in DEFAULT_SETTINGS below.
const unsigned long SHARP_PULL_WINDOW_MS = 600;
const float SHARP_PULL_MIN_DEG = 1.0;    // plus: faster than a wave can move the tip

// ---------- Slow pull / drop-back bucket ----------

const float BUCKET_MIN = 0.80;           // deg*s, smallest bucket size
const float DEFAULT_WAVE_PERIOD = 6.0;   // s, used until the real period is measured
const float WAVE_FILL_TAU = 300.0;       // s, how long a big wave is remembered
const float SUSTAIN_PERIOD_PART = 0.60;  // bucket must stay filled this part of a wave period (a wave can't)
const float GUST_SIDE_MIN = 0.50;        // deg, sideways move that counts as a gust
const float GUST_FACTOR = 0.40;          // bucket fills slower during a gust

// ---------- Settling ----------

const float SETTLE_TOL_MIN = 0.60;       // deg, allowed wobble while settling
const unsigned long SETTLE_TIME_MIN = 1500;
const unsigned long SETTLE_TIME_MAX = 10000;
const float SETTLE_PULL_MIN = 1.50;      // deg moved in 1 s while settling
const unsigned long SETTLE_PULL_TIME = 700;
const unsigned long READY_GRACE_MS = 5000; // ignore knocks after READY (handling)
const unsigned long SETTLE_MAX_WAIT = 8000; // never settle longer than this (after grace)
const float RECENT_REST_TAU = 2.0;       // s, average position used if settling times out
const unsigned long FAST_FOLLOW_MS = 15000; // after a timed-out settle, follow the rod faster
const float FAST_FOLLOW_TAU = 6.0;       // s

// ---------- Pull test ----------

const float PULLTEST_MIN_DEG = 1.0;      // plus 1.5 x the usual wave bend, so a wave can't pass the test
const unsigned long PULLTEST_CONFIRM_MS = 150;
const unsigned long PULLTEST_SETTLE_MS = 1500; // finish this long after the biggest bend

// ---------- Condition labels ----------

const float WAVE_SMALL = 0.30;           // deg swing
const float WAVE_MEDIUM = 1.00;
const float WAVE_BIG = 2.50;

const float WIND_NORMAL = 0.30;          // deg sideways swing
const float WIND_STRONG = 1.00;
const float WIND_VSTRONG = 2.50;

const float WINDVIB_NORMAL = 0.005;      // g vibration ABOVE the sensor's own noise
const float WINDVIB_STRONG = 0.010;
const float WINDVIB_VSTRONG = 0.020;
const float SENSOR_FLOOR_LIS3DH = 0.015; // g, sensor's own noise until measured
const float SENSOR_FLOOR_ADXL355 = 0.002;
float SENSOR_FLOOR_DEFAULT = SENSOR_FLOOR_LIS3DH; // set in setup() for the sensor found
const bool FORGET_SAVED_NOISE = false;   // set true once after changing the sensor board

const float TIDE_SLOW = 0.30;            // deg per minute
const float TIDE_FAST = 1.50;

// ---------- Sea state profiles ----------

struct SeaProfile
{
  float knockDev;        // knock threshold = noise + knockDev * spread
  float allowMul;        // pull allowance = allowMul * wave swing
  float bucketMul;       // bucket = bucketMul x the most the waves filled it
  float holdMul;         // pull time setting x this (rougher sea = a bit longer)
  float noDirAllowMul;   // allowance when the pull direction is not known
  float sharpAlone;      // knock this strong = sharp strike on its own
  float sharpWithPull;   // knock this strong + fast tip pull = sharp strike
};

const SeaProfile SEA_PROFILES[3] =
{
  // CALM
  { 6.0, 0.50, 2.0, 1.0, 1.0, 6.0, 2.5 },

  // MODERATE
  { 7.0, 0.60, 2.2, 1.2, 1.2, 6.5, 2.5 },

  // ROUGH
  { 8.0, 0.70, 2.5, 1.5, 1.5, 7.0, 3.0 }
};

const char* SEA_NAMES[3] = { "calm", "mod", "rough" };

// ---------- Live settings (changed from the dashboard) ----------

struct UserSettings
{
  float knockMul;        // knock limit x this (below 1 = more sensitive)
  float pullDeg;         // deg, tip must lean at least this far to count as a pull
  float pullTime;        // s, a pull must last at least this long
  float repeatCount;     // knocks for a REPEATED BITE
  float repeatWindow;    // s, within this time
  float sharpG;          // knock energy (g) that is always a SHARP STRIKE (~3x = hit size)
  float dropOn;          // 1 = drop-back alarm on
  float knockBeep;       // 1 = single knock beep on
};

const UserSettings DEFAULT_SETTINGS =
{
  1.0,    // knockMul
  1.0,    // pullDeg
  2.0,    // pullTime
  3,      // repeatCount
  4.0,    // repeatWindow
  0.08,   // sharpG
  1,      // dropOn
  1       // knockBeep
};

struct SettingInfo
{
  const char* key;
  float UserSettings::* field;
  float low;
  float high;
};

const SettingInfo SETTING_INFO[] =
{
  { "kmul",  &UserSettings::knockMul,     0.5,  4.0 },
  { "pdeg",  &UserSettings::pullDeg,      0.4,  5.0 },
  { "ptime", &UserSettings::pullTime,     0.5,  8.0 },
  { "rcnt",  &UserSettings::repeatCount,  2,    5 },
  { "rwin",  &UserSettings::repeatWindow, 2.0,  8.0 },
  { "sharp", &UserSettings::sharpG,       0.03, 0.30 },
  { "drop",  &UserSettings::dropOn,       0,    1 },
  { "kbeep", &UserSettings::knockBeep,    0,    1 }
};

const int SETTING_COUNT = sizeof(SETTING_INFO) / sizeof(SETTING_INFO[0]);

UserSettings settings = DEFAULT_SETTINGS;
UserSettings savedSettings = DEFAULT_SETTINGS;

// =====================================================
// VECTOR HELPERS
// =====================================================

const float RAD_TO_DEGREES = 57.29578;

struct Vec3
{
  float x;
  float y;
  float z;
};

Vec3 vec(float x, float y, float z)
{
  Vec3 v = { x, y, z };
  return v;
}

Vec3 vAdd(Vec3 a, Vec3 b) { return vec(a.x + b.x, a.y + b.y, a.z + b.z); }
Vec3 vSub(Vec3 a, Vec3 b) { return vec(a.x - b.x, a.y - b.y, a.z - b.z); }
Vec3 vScale(Vec3 a, float s) { return vec(a.x * s, a.y * s, a.z * s); }
float vDot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float vLen(Vec3 a) { return sqrt(vDot(a, a)); }

Vec3 vCross(Vec3 a, Vec3 b)
{
  return vec(
    a.y * b.z - a.z * b.y,
    a.z * b.x - a.x * b.z,
    a.x * b.y - a.y * b.x
  );
}

Vec3 vNorm(Vec3 a)
{
  float n = vLen(a);

  if(n < 0.000001)
    return vec(0, 0, 1);

  return vScale(a, 1.0 / n);
}

// Part of a that is perpendicular to unit vector n
Vec3 vPerp(Vec3 a, Vec3 n)
{
  return vSub(a, vScale(n, vDot(a, n)));
}

float vAngleDeg(Vec3 a, Vec3 b)
{
  float c = vDot(a, b);

  if(c > 1) c = 1;
  if(c < -1) c = -1;

  return acos(c) * RAD_TO_DEGREES;
}

float clampFloat(float value, float low, float high)
{
  if(value < low) return low;
  if(value > high) return high;

  return value;
}

// Exponential smoothing factor for a time constant
float alphaFor(float tau)
{
  return DT / (tau + DT);
}

// =====================================================
// STATES
// =====================================================

enum RodState
{
  ST_LEARNING,
  ST_PULLTEST,
  ST_MONITORING,
  ST_SETTLING,
  ST_PAUSED
};

RodState rodState = ST_LEARNING;

// Learning started from the watch: pause afterwards so the angler
// can reel in, bait and cast before the pull test
bool learnThenPause = false;

// Running time: since power on, since armed, and all-time total
unsigned long armedSince = 0;            // 0 = not armed
unsigned long totalSecondsSaved = 0;     // all-time seconds saved in flash
unsigned long totalSavedAtMs = 0;        // millis() when it was loaded/saved
const unsigned long TOTAL_SAVE_MS = 600000; // save every 10 min (flash wears out)

bool strikeActive = false;

unsigned long lastSampleTime = 0;

// =====================================================
// SIGNALS
// =====================================================

Vec3 rawAccel = { 0, 0, 1 };
Vec3 gravity = { 0, 0, 1 };      // smoothed accel = where the tip points
Vec3 slowPart = { 0, 0, 1 };     // everything slower than ~4 Hz
Vec3 gravityUnit = { 0, 0, 1 };

float energySq = 0;
float knockEnergy = 0;           // fast vibration strength (g)

float tiltDeg = 0;               // signed: + toward water, - away
float sideDeg = 0;               // sideways (wind)
float totalDeg = 0;              // total angle from rest

float energyPeakSincePoll = 0;

// =====================================================
// REST POSITION + PULL DIRECTION
// =====================================================

Vec3 restDir = { 0, 0, 1 };
Vec3 pullDir = { 1, 0, 0 };      // unit, perpendicular to restDir
Vec3 sideDir = { 0, 1, 0 };

bool haveDir = false;

Vec3 storedDir = { 0, 0, 0 };    // last pull test result, sensor frame
bool haveStoredDir = false;

unsigned long dirLearnedAt = 0;

// =====================================================
// ENVIRONMENT
// =====================================================

float knockMean = 0.005;
float knockSpread = 0.002;

// The sensor's own noise (quietest vibration level ever seen),
// saved so that a still sensor always reads "Calm"
float sensorFloor = SENSOR_FLOOR_DEFAULT;
bool haveSensorFloor = false;
float savedSensorFloor = 0;
unsigned long sensorFloorSavedAt = 0;
float windVib = 0;               // vibration above the sensor's own noise
float knockThreshold = KNOCK_FLOOR;

float gustNoise = 0.005;         // vibration level over the last few seconds

float waveAmp = 0;               // average |tilt|
float waveSwing = 0;             // typical peak swing (deg)
float wavePeriod = 0;            // seconds, 0 = unknown

float sideAmp = 0;
float windSwing = 0;

float tideRate = 0;              // deg per minute
Vec3 tidePrevRest = { 0, 0, 1 };
unsigned long tideCheckTime = 0;

// wave period from upward zero crossings
bool waveBelow = false;
unsigned long lastUpCross = 0;
unsigned long lastWaveCrossSeen = 0;

int waveLevel = 0;
int windLevel = 0;
int tideLevel = 0;

int seaState = 1;                // 0 calm, 1 moderate, 2 rough
bool seaAuto = true;

int seaCandidate = 1;
unsigned long seaCandidateSince = 0;

// =====================================================
// ENVIRONMENT LEARNING
// =====================================================

unsigned long learningStart = 0;

const int LEARN_MAX = LEARN_TIME / SAMPLE_MS + 20;

float learnEnergy[LEARN_MAX];
Vec3 learnDir[LEARN_MAX];
int learnCount = 0;

Vec3 learnSum = { 0, 0, 0 };

// =====================================================
// KNOCK DETECTOR
// =====================================================

bool knockActive = false;
int knockAbove = 0;

unsigned long knockStart = 0;
unsigned long lastKnockEnd = 0;
unsigned long lastValidKnock = 0;

float knockPeak = 0;
float knockTiltAtStart = 0;

float lastKnockStrength = 0;

unsigned long knockTimes[8];
int knockTimeIndex = 0;

unsigned long knockAlertID = 0;

bool sharpPending = false;
unsigned long sharpPendingStart = 0;
float sharpPendingTilt = 0;

// =====================================================
// SLOW PULL / DROP-BACK
// =====================================================

float pullAllow = 1.0;
float bucketSize = BUCKET_MIN;

float pullBucket = 0;
float dropBucket = 0;

// How full the waves alone get the bucket. Learned from each
// wave that fills and empties it without causing an alarm.
float sustainNeed = 1.0;         // s the bucket must stay filled before an alarm

float waveFill = 0;
float pullEpisodePeak = 0;
float dropEpisodePeak = 0;
unsigned long pullEpisodeStart = 0;
unsigned long dropEpisodeStart = 0;

// =====================================================
// SETTLING
// =====================================================

bool pullTestAfterSettle = false;
unsigned long settleGraceUntil = 0;
unsigned long settleStart = 0;

Vec3 recentRest = { 0, 0, 1 };   // where the tip pointed over the last ~2 s
unsigned long fastFollowUntil = 0;

bool calmTimerRunning = false;
unsigned long calmStart = 0;

Vec3 settleAnchor = { 0, 0, 1 };
Vec3 settleSum = { 0, 0, 0 };
unsigned long settleSamples = 0;
float settleMaxDev = 0;

int settlingConfidence = 0;

// Local motion while settling (does not use old rest position)
const int SETTLE_RING = 20;      // one entry every 50 ms = 1 s

Vec3 settleRing[SETTLE_RING];
int settleRingIndex = 0;
int settleRingCount = 0;
int settleRingTick = 0;

bool settlePullCandidate = false;
unsigned long settlePullStart = 0;
bool settlePullAway = false;     // moving away from the water = drop-back

// =====================================================
// PULL TEST
// =====================================================

bool pullTestActive = false;
float pullTestWaveDeg = 0;       // usual total bend from waves alone
unsigned long pullTestAboveSince = 0;
unsigned long pullTestMaxTime = 0;
float pullTestMax = 0;
Vec3 pullTestVec = { 0, 0, 0 };

// =====================================================
// STRIKE
// =====================================================

String strikeType = "";
String lastStrikeType = "NONE";

float strikePeakEnergy = 0;
float strikePeakTilt = 0;

unsigned long strikeStart = 0;
unsigned long strikeEventID = 0;

// =====================================================
// STRIKE REPLAY
//
// 4 s before + 4 s after the strike, 50 points per second.
// =====================================================

const int REC_PRE = 200;
const int REC_POST = 200;
const int REC_TOTAL = REC_PRE + REC_POST;

float preEnergy[REC_PRE];
float preTilt[REC_PRE];
int preIndex = 0;
int preCount = 0;

float savedEnergy[REC_TOTAL];
float savedTilt[REC_TOTAL];
int savedCount = 0;
int savedPre = 0;

float savedThreshold = 0;
float savedAllow = 0;
String savedType = "";

bool recordingStrike = false;
bool lastStrikeReady = false;
int postRecorded = 0;

bool recordTick = false;
float recordEnergyMax = 0;

// =====================================================
// SENSOR
// =====================================================

// Several sensor readings are averaged into each 10 ms step.
// Random noise shrinks, knocks (5-15 Hz) are not affected.
Vec3 oversampleSum = { 0, 0, 0 };
int oversampleCount = 0;
unsigned long lastOversample = 0;

bool sensorHighRes = false;
uint8_t sensorReg1 = 0;
uint8_t sensorReg4 = 0;

void enableHighResolution();

// ---------- Which sensor ----------

enum SensorType
{
  SENSOR_NONE,
  SENSOR_LIS3DH,
  SENSOR_ADXL355
};

SensorType sensorType = SENSOR_NONE;

const char* sensorName()
{
  if(sensorType == SENSOR_ADXL355) return "ADXL355";
  if(sensorType == SENSOR_LIS3DH) return "LIS3DH";

  return "none";
}

// ---------- ADXL355 over I2C ----------

const uint8_t ADXL_DEVID_AD = 0x00;      // AD 1D ED
const uint8_t ADXL_XDATA3 = 0x08;        // X, Y, Z: 3 bytes each, 20 bits
const uint8_t ADXL_FILTER = 0x28;
const uint8_t ADXL_RANGE = 0x2C;
const uint8_t ADXL_POWER_CTL = 0x2D;
const uint8_t ADXL_RESET = 0x2F;
const float ADXL_LSB_PER_G = 256000.0;   // +-2 g range

uint8_t adxlAddr = 0;
unsigned long sensorReadErrors = 0;

bool i2cPresent(uint8_t addr)
{
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool adxlWrite(uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(adxlAddr);
  Wire.write(reg);
  Wire.write(value);

  return Wire.endTransmission() == 0;
}

bool adxlRead(uint8_t reg, uint8_t* buf, uint8_t n)
{
  Wire.beginTransmission(adxlAddr);
  Wire.write(reg);

  if(Wire.endTransmission(false) != 0)
    return false;

  if(Wire.requestFrom(adxlAddr, n) != n)
    return false;

  for(uint8_t i = 0; i < n; i++)
    buf[i] = Wire.read();

  return true;
}

float adxlAxis(const uint8_t* b)
{
  long v = ((long)b[0] << 12) | ((long)b[1] << 4) | (b[2] >> 4);

  if(v & 0x80000)
    v -= 0x100000;

  return v / ADXL_LSB_PER_G;
}

bool startADXL355()
{
  if(i2cPresent(0x53)) adxlAddr = 0x53;
  else if(i2cPresent(0x1D)) adxlAddr = 0x1D;
  else return false;

  uint8_t id[3] = { 0, 0, 0 };
  adxlRead(ADXL_DEVID_AD, id, 3);

  if(id[0] != 0xAD || id[1] != 0x1D || id[2] != 0xED)
  {
    Serial.printf("Device at 0x%02X is not an ADXL355 (ID %02X %02X %02X)\n", adxlAddr, id[0], id[1], id[2]);
    return false;
  }

  adxlWrite(ADXL_RESET, 0x52);
  delay(50);

  adxlWrite(ADXL_RANGE, 0x01);       // +-2 g, the most sensitive
  adxlWrite(ADXL_FILTER, 0x03);      // 500 readings per second (filter 125 Hz)
  adxlWrite(ADXL_POWER_CTL, 0x00);   // start measuring

  delay(50);

  sensorType = SENSOR_ADXL355;
  sensorHighRes = true;              // always 20 bit

  Serial.printf("ADXL355 CONNECTED at 0x%02X\n", adxlAddr);

  return true;
}

bool startLIS3DH()
{
  myIMU.settings.accelSampleRate = SENSOR_RATE;
  myIMU.settings.accelRange = 2;          // most sensitive range

  if(myIMU.begin() != 0)
    return false;

  // After begin(), which may reset the bus speed
  Wire.setClock(400000);

  sensorType = SENSOR_LIS3DH;

  Serial.println("LIS3DH CONNECTED");

  enableHighResolution();

  return true;
}

bool startSensor()
{
  if(startADXL355())
  {
    KNOCK_FLOOR = KNOCK_FLOOR_ADXL355;
    SENSOR_FLOOR_DEFAULT = SENSOR_FLOOR_ADXL355;
    return true;
  }

  if(startLIS3DH())
  {
    KNOCK_FLOOR = KNOCK_FLOOR_LIS3DH;
    SENSOR_FLOOR_DEFAULT = SENSOR_FLOOR_LIS3DH;
    return true;
  }

  return false;
}

// One reading in g. Returns false if the sensor did not answer.
bool readOnce(Vec3& out)
{
  if(sensorType == SENSOR_ADXL355)
  {
    uint8_t b[9];

    if(!adxlRead(ADXL_XDATA3, b, 9))
    {
      sensorReadErrors++;
      return false;
    }

    out.x = adxlAxis(b);
    out.y = adxlAxis(b + 3);
    out.z = adxlAxis(b + 6);

    return true;
  }

  out.x = myIMU.readFloatAccelX();
  out.y = myIMU.readFloatAccelY();
  out.z = myIMU.readFloatAccelZ();

  return true;
}

void oversample()
{
  if(micros() - lastOversample < OVERSAMPLE_US)
    return;

  lastOversample = micros();

  Vec3 a;

  if(!readOnce(a))
    return;

  oversampleSum = vAdd(oversampleSum, a);
  oversampleCount++;
}

void readSensor()
{
  if(oversampleCount == 0)
  {
    readOnce(rawAccel);
    return;
  }

  rawAccel = vScale(oversampleSum, 1.0 / oversampleCount);

  oversampleSum = vec(0, 0, 0);
  oversampleCount = 0;
}

// High resolution (12 bit) mode: HR bit on, low-power bit off.
// Read back so we can see that it really worked.
void enableHighResolution()
{
  uint8_t reg = 0;

  myIMU.readRegister(&reg, LIS3DH_CTRL_REG1);
  reg &= ~0x08;  // LPen off
  myIMU.writeRegister(LIS3DH_CTRL_REG1, reg);

  myIMU.readRegister(&reg, LIS3DH_CTRL_REG4);
  reg |= 0x08;   // HR on
  myIMU.writeRegister(LIS3DH_CTRL_REG4, reg);

  myIMU.readRegister(&sensorReg1, LIS3DH_CTRL_REG1);
  myIMU.readRegister(&sensorReg4, LIS3DH_CTRL_REG4);

  sensorHighRes =
    (sensorReg4 & 0x08) &&
    !(sensorReg1 & 0x08);

  Serial.print("CTRL_REG1=0x");
  Serial.print(sensorReg1, HEX);
  Serial.print(" CTRL_REG4=0x");
  Serial.print(sensorReg4, HEX);
  Serial.println(sensorHighRes ? "  HIGH RESOLUTION ON" : "  HIGH RESOLUTION FAILED");
}

// =====================================================
// SIGNAL PROCESSING
// =====================================================

// ---------- 3D noise view ----------
//
// The last vibration vectors (100 per second), sent to the dashboard
// with every /data answer. Units: 0.1 mg (0.0001 g).

const int VIB_RING = 64;

int16_t vibRing[VIB_RING][3];
unsigned long vibWritten = 0;
unsigned long vibSent = 0;

int16_t toTenthMg(float g)
{
  float v = g * 10000.0;

  if(v > 32000) v = 32000;
  if(v < -32000) v = -32000;

  return (int16_t)lround(v);
}

void storeVibration(Vec3 v)
{
  int i = vibWritten % VIB_RING;

  vibRing[i][0] = toTenthMg(v.x);
  vibRing[i][1] = toTenthMg(v.y);
  vibRing[i][2] = toTenthMg(v.z);

  vibWritten++;
}

void addVibrationSamples(String& json)
{
  unsigned long from = vibSent;

  if(vibWritten - from > VIB_RING)
    from = vibWritten - VIB_RING;

  json += ",\"vib\":[";

  for(unsigned long n = from; n < vibWritten; n++)
  {
    int i = n % VIB_RING;

    if(n != from) json += ",";

    json += vibRing[i][0];
    json += ",";
    json += vibRing[i][1];
    json += ",";
    json += vibRing[i][2];
  }

  json += "],\"g\":[";
  json += String(gravityUnit.x, 3);
  json += ",";
  json += String(gravityUnit.y, 3);
  json += ",";
  json += String(gravityUnit.z, 3);
  json += "]";

  vibSent = vibWritten;
}

void updateSignals()
{
  gravity =
    vAdd(
      gravity,
      vScale(
        vSub(rawAccel, gravity),
        alphaFor(GRAVITY_TAU)
      )
    );

  // Bending of the tip (even a fast one) is not vibration
  slowPart =
    vAdd(
      slowPart,
      vScale(
        vSub(rawAccel, slowPart),
        alphaFor(VIBRATION_TAU)
      )
    );

  Vec3 vibration =
    vSub(rawAccel, slowPart);

  storeVibration(vibration);

  energySq +=
    (vDot(vibration, vibration) - energySq) *
    alphaFor(ENERGY_TAU);

  knockEnergy =
    sqrt(energySq);

  if(knockEnergy > energyPeakSincePoll)
    energyPeakSincePoll = knockEnergy;

  // Ignore free fall / bad readings for the angle
  if(vLen(gravity) > 0.5)
    gravityUnit = vNorm(gravity);

  float c = vDot(gravityUnit, restDir);

  totalDeg = vAngleDeg(gravityUnit, restDir);

  if(haveDir)
  {
    tiltDeg =
      atan2(vDot(gravityUnit, pullDir), c) *
      RAD_TO_DEGREES;

    sideDeg =
      atan2(vDot(gravityUnit, sideDir), c) *
      RAD_TO_DEGREES;
  }
  else
  {
    // No direction: fall back to "how much", like the old code
    tiltDeg = totalDeg;
    sideDeg = 0;
  }

  recentRest =
    vNorm(
      vAdd(
        recentRest,
        vScale(
          vSub(gravityUnit, recentRest),
          alphaFor(RECENT_REST_TAU)
        )
      )
    );
}

// =====================================================
// REST POSITION + DIRECTION
// =====================================================

void refreshDirection()
{
  if(!haveStoredDir)
  {
    haveDir = false;
    return;
  }

  Vec3 d =
    vPerp(storedDir, restDir);

  if(vLen(d) < 0.3)
  {
    // Rod is now pointing along the old pull direction
    haveDir = false;
    return;
  }

  pullDir = vNorm(d);
  sideDir = vCross(restDir, pullDir);
  haveDir = true;
}

void setRest(Vec3 r)
{
  restDir = vNorm(r);

  tidePrevRest = restDir;
  tideCheckTime = millis();

  refreshDirection();
}

void loadStoredDirection()
{
  prefs.begin("strikesense", true);

  haveStoredDir = prefs.getBool("hasdir", false);

  storedDir.x = prefs.getFloat("dx", 0);
  storedDir.y = prefs.getFloat("dy", 0);
  storedDir.z = prefs.getFloat("dz", 0);

  prefs.end();

  if(vLen(storedDir) < 0.5)
    haveStoredDir = false;
}

void loadSensorFloor()
{
  prefs.begin("strikesense", true);

  bool adxl = sensorType == SENSOR_ADXL355;

  haveSensorFloor = !FORGET_SAVED_NOISE && prefs.getBool(adxl ? "hasfloorA" : "hasfloor", false);
  savedSensorFloor = prefs.getFloat(adxl ? "floorA" : "floor", SENSOR_FLOOR_DEFAULT);

  prefs.end();

  if(haveSensorFloor)
    sensorFloor = savedSensorFloor;
}

// Flash wears out with many writes: only save a clearly lower
// value, and not more than once a minute
void maybeSaveSensorFloor()
{
  if(haveSensorFloor && sensorFloor > savedSensorFloor * 0.9)
    return;

  if(sensorFloorSavedAt != 0 && millis() - sensorFloorSavedAt < 60000)
    return;

  prefs.begin("strikesense", false);
  bool adxl = sensorType == SENSOR_ADXL355;

  prefs.putBool(adxl ? "hasfloorA" : "hasfloor", true);
  prefs.putFloat(adxl ? "floorA" : "floor", sensorFloor);
  prefs.end();

  haveSensorFloor = true;
  savedSensorFloor = sensorFloor;
  sensorFloorSavedAt = millis();

  Serial.print("Sensor noise saved (g): ");
  Serial.println(sensorFloor, 4);
}

// Lower the sensor noise estimate whenever it is quieter than ever
void updateSensorFloor(float level)
{
  if(!haveSensorFloor || level < sensorFloor)
  {
    sensorFloor = level;
    maybeSaveSensorFloor();
  }
}

// ---------- Live settings in flash ----------

void loadSettings()
{
  prefs.begin("strikesense", true);

  for(int i = 0; i < SETTING_COUNT; i++)
  {
    const SettingInfo& info = SETTING_INFO[i];

    settings.*(info.field) =
      clampFloat(
        prefs.getFloat(info.key, DEFAULT_SETTINGS.*(info.field)),
        info.low,
        info.high
      );
  }

  prefs.end();

  savedSettings = settings;
}

void saveSettings()
{
  prefs.begin("strikesense", false);

  for(int i = 0; i < SETTING_COUNT; i++)
    prefs.putFloat(SETTING_INFO[i].key, settings.*(SETTING_INFO[i].field));

  prefs.end();

  savedSettings = settings;

  Serial.println("SETTINGS SAVED");
}

bool settingsSaved()
{
  for(int i = 0; i < SETTING_COUNT; i++)
  {
    if(settings.*(SETTING_INFO[i].field) != savedSettings.*(SETTING_INFO[i].field))
      return false;
  }

  return true;
}

// ---------- All-time running hours in flash ----------

void loadTotalTime()
{
  prefs.begin("strikesense", true);
  totalSecondsSaved = prefs.getULong("total", 0);
  prefs.end();

  totalSavedAtMs = millis();
}

unsigned long totalSeconds()
{
  return totalSecondsSaved + (millis() - totalSavedAtMs) / 1000;
}

void maybeSaveTotalTime()
{
  if(millis() - totalSavedAtMs < TOTAL_SAVE_MS)
    return;

  totalSecondsSaved = totalSeconds();
  totalSavedAtMs = millis();

  prefs.begin("strikesense", false);
  prefs.putULong("total", totalSecondsSaved);
  prefs.end();
}

void saveStoredDirection()
{
  prefs.begin("strikesense", false);

  prefs.putBool("hasdir", true);
  prefs.putFloat("dx", storedDir.x);
  prefs.putFloat("dy", storedDir.y);
  prefs.putFloat("dz", storedDir.z);

  prefs.end();
}

// =====================================================
// ENVIRONMENT TRACKING
// =====================================================

const SeaProfile& profile()
{
  return SEA_PROFILES[seaState];
}

void updateKnockThreshold()
{
  float thr =
    knockMean + profile().knockDev * knockSpread * settings.knockMul;

  // A wind gust raises the vibration for a few seconds above
  // normal: raise the limit by the same ratio, so knocks must
  // stand out from the gust too
  if(gustNoise > knockMean && knockMean > 0)
    thr *= 1.0 + GUST_NOISE_MUL * (gustNoise / knockMean - 1.0);

  knockThreshold =
    // The slider also moves the smallest limit, so it works
    // even when the sensor is so quiet that the minimum applies
    clampFloat(thr, KNOCK_FLOOR * settings.knockMul, KNOCK_CEIL);
}

// Background vibration. Learns from quiet readings and from
// long shakes (wind), never from real knocks.
void updateKnockNoise()
{
  // Fast level follows everything except knocks and their ring-down
  bool nearKnock =
    (knockActive && millis() - knockStart <= KNOCK_MAX_MS) ||
    millis() - lastValidKnock < 300;

  if(!nearKnock)
  {
    gustNoise +=
      (knockEnergy - gustNoise) * alphaFor(GUST_NOISE_TAU);

    updateKnockThreshold();
  }

  bool quietSample =
    !knockActive &&
    knockEnergy < knockThreshold;

  bool windShake =
    knockActive &&
    millis() - knockStart > KNOCK_MAX_MS;

  if(!quietSample && !windShake)
    return;

  if(millis() - lastValidKnock < 1000)
    return;

  float a = alphaFor(NOISE_TAU);

  knockMean += (knockEnergy - knockMean) * a;

  knockSpread +=
    (fabs(knockEnergy - knockMean) - knockSpread) * a;

  updateKnockThreshold();

  if(rodState == ST_MONITORING)
    updateSensorFloor(knockMean);
}

void updateWaveWindTide()
{
  // ---------- Waves ----------

  waveAmp +=
    (fabs(tiltDeg) - waveAmp) *
    alphaFor(WAVE_TAU);

  // average |sine| = 0.64 * peak
  waveSwing = waveAmp * 1.57;

  // Period from upward zero crossings (signed tilt only)
  if(haveDir)
  {
    float h = waveAmp * 0.5;

    if(h < 0.05) h = 0.05;

    if(tiltDeg < -h)
    {
      waveBelow = true;
    }
    else if(tiltDeg > h && waveBelow)
    {
      waveBelow = false;

      unsigned long now = millis();

      if(lastUpCross != 0)
      {
        float period =
          (now - lastUpCross) / 1000.0;

        if(period >= 1.5 && period <= 20.0)
        {
          if(wavePeriod <= 0)
            wavePeriod = period;
          else
            wavePeriod += (period - wavePeriod) * 0.2;

          lastWaveCrossSeen = now;
        }
      }

      lastUpCross = now;
    }

    if(
      wavePeriod > 0 &&
      millis() - lastWaveCrossSeen > 30000
    )
    {
      wavePeriod = 0;
    }
  }

  // ---------- Wind (sideways) ----------

  if(haveDir)
  {
    sideAmp +=
      (fabs(sideDeg) - sideAmp) *
      alphaFor(WIND_TAU);

    windSwing = sideAmp * 1.57;
  }

  // ---------- Rest position follows tide ----------

  float restTau =
    millis() < fastFollowUntil ? FAST_FOLLOW_TAU : BASELINE_TAU;

  restDir =
    vNorm(
      vAdd(
        restDir,
        vScale(
          vSub(gravityUnit, restDir),
          alphaFor(restTau)
        )
      )
    );

  if(haveDir)
  {
    pullDir = vNorm(vPerp(pullDir, restDir));
    sideDir = vCross(restDir, pullDir);
  }

  // ---------- Tide drift rate ----------

  if(millis() - tideCheckTime >= 10000)
  {
    float moved =
      vAngleDeg(restDir, tidePrevRest);

    float rate = moved * 6.0;    // per 10 s -> per minute

    tideRate += (rate - tideRate) * 0.3;

    tidePrevRest = restDir;
    tideCheckTime = millis();
  }
}

int levelFor(float value, float a, float b, float c)
{
  if(value < a) return 0;
  if(value < b) return 1;
  if(value < c) return 2;

  return 3;
}

int autoSeaState()
{
  if(waveLevel >= 3 || windLevel >= 3)
    return 2;

  if(waveLevel <= 1 && windLevel <= 1)
    return 0;

  return 1;
}

void updateConditions(bool immediate)
{
  waveLevel =
    levelFor(waveSwing, WAVE_SMALL, WAVE_MEDIUM, WAVE_BIG);

  // Noise adds up like the sides of a right triangle, so remove
  // the sensor's own noise that way
  float extra = knockMean * knockMean - sensorFloor * sensorFloor;

  windVib = extra > 0 ? sqrt(extra) : 0;

  int vibLevel =
    levelFor(windVib, WINDVIB_NORMAL, WINDVIB_STRONG, WINDVIB_VSTRONG);

  windLevel = vibLevel;

  if(haveDir)
  {
    int swayLevel =
      levelFor(windSwing, WIND_NORMAL, WIND_STRONG, WIND_VSTRONG);

    if(swayLevel > windLevel)
      windLevel = swayLevel;
  }

  if(tideRate < TIDE_SLOW) tideLevel = 0;
  else if(tideRate < TIDE_FAST) tideLevel = 1;
  else tideLevel = 2;

  if(!seaAuto)
    return;

  int wanted = autoSeaState();

  if(immediate)
  {
    seaState = wanted;
    seaCandidate = wanted;
    updateKnockThreshold();

    return;
  }

  if(wanted == seaState)
  {
    seaCandidate = seaState;
    return;
  }

  if(wanted != seaCandidate)
  {
    seaCandidate = wanted;
    seaCandidateSince = millis();
  }
  else if(millis() - seaCandidateSince >= SEA_STATE_HOLD)
  {
    seaState = wanted;
    updateKnockThreshold();

    Serial.print("SEA STATE: ");
    Serial.println(SEA_NAMES[seaState]);
  }
}

// =====================================================
// STRIKE REPLAY RECORDING
// =====================================================

void startStrikeRecording()
{
  savedCount = 0;

  int start =
    preCount < REC_PRE ? 0 : preIndex;

  for(int i = 0; i < preCount; i++)
  {
    int index = (start + i) % REC_PRE;

    savedEnergy[savedCount] = preEnergy[index];
    savedTilt[savedCount] = preTilt[index];
    savedCount++;
  }

  savedPre = savedCount;
  savedThreshold = knockThreshold;
  savedAllow = pullAllow;
  savedType = strikeType;

  recordingStrike = true;
  postRecorded = 0;
  lastStrikeReady = false;
}

// Called every reading, stores every second reading (50 per second)
void updateRecording()
{
  if(knockEnergy > recordEnergyMax)
    recordEnergyMax = knockEnergy;

  recordTick = !recordTick;

  if(!recordTick)
    return;

  float e = recordEnergyMax;
  recordEnergyMax = 0;

  if(recordingStrike)
  {
    if(savedCount < REC_TOTAL)
    {
      savedEnergy[savedCount] = e;
      savedTilt[savedCount] = tiltDeg;
      savedCount++;
    }

    postRecorded++;

    if(postRecorded >= REC_POST || savedCount >= REC_TOTAL)
    {
      recordingStrike = false;
      lastStrikeReady = true;
    }
  }

  preEnergy[preIndex] = e;
  preTilt[preIndex] = tiltDeg;

  preIndex++;

  if(preIndex >= REC_PRE)
    preIndex = 0;

  if(preCount < REC_PRE)
    preCount++;
}

// =====================================================
// DETECTOR RESET
// =====================================================

void resetDetectors()
{
  knockActive = false;
  knockAbove = 0;

  for(int i = 0; i < 8; i++)
    knockTimes[i] = 0;

  sharpPending = false;

  pullBucket = 0;
  dropBucket = 0;

  pullEpisodePeak = 0;
  dropEpisodePeak = 0;
  pullEpisodeStart = 0;
  dropEpisodeStart = 0;
}

void resetSettleWindow()
{
  calmTimerRunning = false;

  settleSum = vec(0, 0, 0);
  settleSamples = 0;
  settleMaxDev = 0;

  settlingConfidence = 0;
}

void resetSettleRing()
{
  settleRingIndex = 0;
  settleRingCount = 0;
  settleRingTick = 0;

  settlePullCandidate = false;
}

// =====================================================
// STRIKE
// =====================================================

void startStrike(String type)
{
  if(strikeActive)
    return;

  if(rodState == ST_PAUSED)
    return;

  strikeActive = true;

  strikeType = type;
  lastStrikeType = type;

  strikeEventID++;

  strikePeakEnergy = knockEnergy;
  strikePeakTilt = fabs(tiltDeg);

  strikeStart = millis();

  // A strike during settling ends settling
  if(rodState == ST_SETTLING)
  {
    rodState = ST_MONITORING;
    resetSettleWindow();
    resetSettleRing();
  }

  sharpPending = false;

  startStrikeRecording();

  loraOnStrike();

  Serial.println();
  Serial.print(">>> STRIKE: ");
  Serial.print(type);
  Serial.println(" <<<");
}

void clearStrike()
{
  strikeActive = false;

  strikeType = "";

  strikePeakEnergy = 0;
  strikePeakTilt = 0;
}

// =====================================================
// STATE CHANGES
// =====================================================

void goMonitoring()
{
  rodState = ST_MONITORING;

  resetDetectors();
  resetSettleWindow();
  resetSettleRing();

  settlingConfidence = 100;

  if(armedSince == 0)
    armedSince = millis();

  Serial.println("MONITORING");
}

void startPullTest()
{
  rodState = ST_PULLTEST;

  // After learning this is already measured; after READY use the wave tracker
  if(pullTestWaveDeg <= 0)
    pullTestWaveDeg = waveSwing;

  pullTestActive = false;
  pullTestAboveSince = 0;
  pullTestMax = 0;

  resetDetectors();

  Serial.println();
  Serial.println("PULL TEST: pull the line gently once");
}

void startSettling(bool fromReady)
{
  rodState = ST_SETTLING;

  pullTestAfterSettle = fromReady;

  settleGraceUntil =
    fromReady ? millis() + READY_GRACE_MS : 0;

  settleStart = millis();

  resetSettleWindow();
  resetSettleRing();
  resetDetectors();

  Serial.println();
  Serial.println("PROTECTED SETTLING STARTED");
}

void startEnvironmentLearning()
{
  rodState = ST_LEARNING;

  learnThenPause = false;

  armedSince = 0;

  clearStrike();
  resetDetectors();
  resetSettleWindow();
  resetSettleRing();

  readSensor();

  gravity = rawAccel;
  slowPart = rawAccel;
  gravityUnit = vNorm(gravity);
  recentRest = gravityUnit;
  energySq = 0;
  gustNoise = 0.005;

  learnCount = 0;
  learnSum = vec(0, 0, 0);

  waveAmp = 0;
  waveSwing = 0;
  wavePeriod = 0;
  sideAmp = 0;
  windSwing = 0;
  tideRate = 0;

  lastUpCross = 0;
  waveBelow = false;

  learningStart = millis();

  Serial.println();
  Serial.println("ENVIRONMENT LEARNING STARTED");
}

// =====================================================
// LEARNING STEP
// =====================================================

int compareFloat(const void* a, const void* b)
{
  float fa = *(const float*)a;
  float fb = *(const float*)b;

  if(fa < fb) return -1;
  if(fa > fb) return 1;

  return 0;
}

void learningStep()
{
  unsigned long elapsed =
    millis() - learningStart;

  // Give the filters 0.5 s to settle first
  if(elapsed > 500 && learnCount < LEARN_MAX)
  {
    learnEnergy[learnCount] = knockEnergy;
    learnDir[learnCount] = gravityUnit;
    learnCount++;

    learnSum = vAdd(learnSum, gravityUnit);
  }

  if(elapsed < LEARN_TIME || learnCount < 10)
    return;

  // ---------- Background vibration: median and 95% ----------

  qsort(learnEnergy, learnCount, sizeof(float), compareFloat);

  float p50 = learnEnergy[learnCount / 2];
  float p95 = learnEnergy[(learnCount * 95) / 100];

  knockMean = p50;
  gustNoise = p50;

  updateSensorFloor(p50);
  knockSpread = (p95 - p50) / 2.0;

  if(knockSpread < 0.0005)
    knockSpread = 0.0005;

  // ---------- Rest position: average ----------

  Vec3 mean = vScale(learnSum, 1.0 / learnCount);

  setRest(mean);

  // ---------- Starting wave + wind size ----------

  float sumWave = 0;
  float sumSide = 0;

  for(int i = 0; i < learnCount; i++)
  {
    Vec3 g = learnDir[i];
    float c = vDot(g, restDir);

    if(haveDir)
    {
      sumWave += fabs(atan2(vDot(g, pullDir), c));
      sumSide += fabs(atan2(vDot(g, sideDir), c));
    }
    else
    {
      sumWave += acos(clampFloat(c, -1, 1));
    }
  }

  waveAmp = sumWave / learnCount * RAD_TO_DEGREES;
  waveSwing = waveAmp * 1.57;

  // Usual bend from waves (90% of readings) for the pull test.
  // learnEnergy is not needed any more, reuse it.
  for(int i = 0; i < learnCount; i++)
    learnEnergy[i] = vAngleDeg(learnDir[i], restDir);

  qsort(learnEnergy, learnCount, sizeof(float), compareFloat);

  pullTestWaveDeg = learnEnergy[(learnCount * 90) / 100];

  sideAmp = sumSide / learnCount * RAD_TO_DEGREES;
  windSwing = sideAmp * 1.57;

  // Starting guess for how full one wave gets the bucket,
  // replaced by real measurements after a few waves
  waveFill = 0.3 * DEFAULT_WAVE_PERIOD * waveSwing;

  updateConditions(true);
  updateKnockThreshold();

  Serial.println();
  Serial.println("LEARNING COMPLETE");
  Serial.print("Knock threshold (g): ");
  Serial.println(knockThreshold, 4);
  Serial.print("Wave swing (deg): ");
  Serial.println(waveSwing, 2);
  Serial.print("Sea state: ");
  Serial.println(SEA_NAMES[seaState]);

  if(learnThenPause)
  {
    learnThenPause = false;
    rodState = ST_PAUSED;

    Serial.println("PAUSED - cast, then READY on the watch");
    return;
  }

  startPullTest();
}

// =====================================================
// PULL TEST STEP
// =====================================================

float pullTestNeed()
{
  return PULLTEST_MIN_DEG + 1.5 * pullTestWaveDeg;
}

void pullTestStep()
{
  float needed = pullTestNeed();

  if(!pullTestActive)
  {
    if(totalDeg > needed)
    {
      if(pullTestAboveSince == 0)
        pullTestAboveSince = millis();

      if(millis() - pullTestAboveSince >= PULLTEST_CONFIRM_MS)
      {
        pullTestActive = true;
        pullTestMax = 0;
        pullTestMaxTime = millis();
        pullTestVec = vec(0, 0, 0);
      }
    }
    else
    {
      pullTestAboveSince = 0;
    }

    return;
  }

  if(totalDeg > pullTestMax)
  {
    pullTestMax = totalDeg;
    pullTestMaxTime = millis();
  }

  // Average the bend over the whole pull, big bends count most,
  // so one wave at the wrong moment can't spoil the direction
  pullTestVec =
    vAdd(
      pullTestVec,
      vScale(vPerp(gravityUnit, restDir), totalDeg * totalDeg)
    );

  // Pull finished when the tip comes back, or (in waves)
  // when the bend stopped growing
  if(
    totalDeg < pullTestMax * 0.4 ||
    millis() - pullTestMaxTime >= PULLTEST_SETTLE_MS
  )
  {
    storedDir = vNorm(pullTestVec);
    haveStoredDir = true;

    saveStoredDirection();
    refreshDirection();

    dirLearnedAt = millis();

    Serial.print("PULL DIRECTION LEARNED (");
    Serial.print(pullTestMax, 1);
    Serial.println(" deg)");

    goMonitoring();
  }
}

// =====================================================
// KNOCK DETECTOR
//
// Returns true when a valid knock just ended.
// =====================================================

bool updateKnockDetector()
{
  unsigned long now = millis();

  // Must stay above the normal noise, or a knock never ends
  float releaseLevel =
    knockMean + (knockThreshold - knockMean) * KNOCK_RELEASE;

  if(!knockActive)
  {
    if(
      now - lastKnockEnd >= KNOCK_GAP_MS &&
      knockEnergy > knockThreshold
    )
    {
      knockAbove++;

      if(knockAbove >= KNOCK_CONFIRM)
      {
        knockActive = true;
        knockStart = now - SAMPLE_MS * (KNOCK_CONFIRM - 1);
        knockPeak = knockEnergy;
        knockTiltAtStart = tiltDeg;
      }
    }
    else
    {
      knockAbove = 0;
    }

    return false;
  }

  if(knockEnergy > knockPeak)
    knockPeak = knockEnergy;

  unsigned long duration =
    now - knockStart;

  bool ended =
    knockEnergy < releaseLevel;

  // Never stay stuck in a long shake
  if(!ended && duration > 3000)
    ended = true;

  if(!ended)
    return false;

  knockActive = false;
  knockAbove = 0;
  lastKnockEnd = now;

  if(duration > KNOCK_MAX_MS)
    return false;      // wind gust or handling, not a knock

  lastKnockStrength =
    knockPeak / knockThreshold;

  lastValidKnock = now;

  knockTimes[knockTimeIndex] = now;
  knockTimeIndex = (knockTimeIndex + 1) % 8;

  return true;
}

int recentKnockCount()
{
  unsigned long now = millis();

  int count = 0;

  for(int i = 0; i < 8; i++)
  {
    if(
      knockTimes[i] != 0 &&
      now - knockTimes[i] <= settings.repeatWindow * 1000.0
    )
    {
      count++;
    }
  }

  return count;
}

void onKnock(bool settlingMode)
{
  const SeaProfile& p = profile();

  if(settlingMode)
  {
    // Right after READY the angler is still handling the rod
    if(millis() < settleGraceUntil)
      return;

    if(lastKnockStrength >= p.sharpAlone || knockPeak >= settings.sharpG)
    {
      startStrike("SHARP STRIKE");
      return;
    }

    knockAlertID++;
    return;
  }

  if(lastKnockStrength >= p.sharpAlone || knockPeak >= settings.sharpG)
  {
    startStrike("SHARP STRIKE");
    return;
  }

  if(recentKnockCount() >= (int)settings.repeatCount)
  {
    startStrike("REPEATED BITE");
    return;
  }

  if(lastKnockStrength >= p.sharpWithPull)
  {
    // Strong knock: sharp strike if the tip also pulls down
    sharpPending = true;
    sharpPendingStart = millis();
    sharpPendingTilt = knockTiltAtStart;
  }

  knockAlertID++;
}

// A very strong shake is a strike straight away, however long it lasts
bool checkViolentShake()
{
  if(!knockActive)
    return false;

  float level = profile().sharpAlone * knockThreshold;

  if(level > settings.sharpG)
    level = settings.sharpG;

  if(knockEnergy < level)
    return false;

  knockPeak = knockEnergy;
  startStrike("SHARP STRIKE");

  return true;
}

void checkSharpPending()
{
  if(!sharpPending)
    return;

  if(millis() - sharpPendingStart > SHARP_PULL_WINDOW_MS)
  {
    sharpPending = false;
    return;
  }

  // In 0.6 s a wave moves the tip at most ~0.75 x its swing
  float needed = SHARP_PULL_MIN_DEG;

  if(waveSwing * 0.8 > needed)
    needed = waveSwing * 0.8;

  if(tiltDeg - sharpPendingTilt >= needed)
  {
    startStrike("SHARP STRIKE");
  }
}

// =====================================================
// SLOW PULL + DROP-BACK (bucket / CUSUM)
// =====================================================

// A wave fills the bucket a little and empties it again within
// part of a wave period. When it is empty again, remember how
// full that wave got it. Long episodes are not waves (a fish
// that pulled but not enough) and are not learned.
void learnWaveFill(float bucket, float& episodePeak, unsigned long& episodeStart)
{
  if(bucket > 0 && episodeStart == 0)
    episodeStart = millis();

  if(bucket > episodePeak)
    episodePeak = bucket;

  if(bucket > 0 || episodeStart == 0)
    return;

  float period =
    wavePeriod > 0 ? wavePeriod : DEFAULT_WAVE_PERIOD;

  float maxLength = period * 0.8;

  if(maxLength < 3.0)
    maxLength = 3.0;

  bool waveLike =
    (millis() - episodeStart) / 1000.0 <= maxLength;

  if(waveLike && episodePeak > waveFill)
    waveFill += (episodePeak - waveFill) * 0.5;

  episodePeak = 0;
  episodeStart = 0;
}

// How long the bucket has been filled without emptying (s)
float episodeLength(unsigned long episodeStart)
{
  if(episodeStart == 0)
    return 0;

  return (millis() - episodeStart) / 1000.0;
}

void updatePullBuckets()
{
  const SeaProfile& p = profile();

  if(haveDir)
  {
    // Waves go + then -, so a small allowance is enough: each wave
    // pours a little into the bucket and then drains it again.
    // The bucket is made clearly bigger than the waves ever fill it.
    pullAllow = p.allowMul * waveSwing;
    bucketSize = p.bucketMul * waveFill;
  }
  else
  {
    // Only "how much" is known: the allowance must be above the waves
    pullAllow = p.noDirAllowMul * waveSwing;
    bucketSize = 0;
  }

  if(pullAllow < settings.pullDeg)
    pullAllow = settings.pullDeg;

  float pullTime = settings.pullTime * p.holdMul;

  // A pull of 1.5x the allowance fills it in the pull time
  if(bucketSize < pullAllow * pullTime * 0.5)
    bucketSize = pullAllow * pullTime * 0.5;

  if(bucketSize < BUCKET_MIN)
    bucketSize = BUCKET_MIN;

  // A wave fills the bucket and empties it again within about half
  // a wave period. A fish keeps it filled. When there are waves,
  // the bucket must stay filled longer than a wave can manage.
  sustainNeed = pullTime;

  if(waveSwing > WAVE_SMALL)
  {
    float period =
      wavePeriod > 0 ? wavePeriod : DEFAULT_WAVE_PERIOD;

    float crest = period * SUSTAIN_PERIOD_PART;

    if(crest > sustainNeed)
      sustainNeed = crest;
  }

  // ---------- Toward water ----------

  float excess = tiltDeg - pullAllow;

  if(excess > 0 && haveDir)
  {
    float gustLevel = windSwing * 2.5;

    if(gustLevel < GUST_SIDE_MIN)
      gustLevel = GUST_SIDE_MIN;

    if(fabs(sideDeg) > gustLevel)
      excess *= GUST_FACTOR;
  }

  pullBucket += excess * DT;

  pullBucket = clampFloat(pullBucket, 0, bucketSize);

  learnWaveFill(pullBucket, pullEpisodePeak, pullEpisodeStart);

  if(
    pullBucket >= bucketSize &&
    episodeLength(pullEpisodeStart) >= sustainNeed
  )
  {
    startStrike("SLOW PULL");
    return;
  }

  // ---------- Away from water ----------

  if(!haveDir || settings.dropOn < 0.5)
  {
    dropBucket = 0;
    dropEpisodeStart = 0;
    return;
  }

  float dropExcess = -tiltDeg - pullAllow;

  dropBucket += dropExcess * DT;

  dropBucket = clampFloat(dropBucket, 0, bucketSize);

  learnWaveFill(dropBucket, dropEpisodePeak, dropEpisodeStart);

  if(
    dropBucket >= bucketSize &&
    episodeLength(dropEpisodeStart) >= sustainNeed
  )
  {
    startStrike("DROP-BACK");
  }
}

// =====================================================
// MONITORING STEP
// =====================================================

void monitoringStep()
{
  bool knock = updateKnockDetector();

  if(strikeActive)
  {
    if(knockEnergy > strikePeakEnergy)
      strikePeakEnergy = knockEnergy;

    if(fabs(tiltDeg) > strikePeakTilt)
      strikePeakTilt = fabs(tiltDeg);

    return;
  }

  updateKnockNoise();

  if(checkViolentShake())
    return;

  if(knock)
  {
    onKnock(false);

    if(strikeActive)
      return;
  }

  checkSharpPending();

  if(strikeActive)
    return;

  updatePullBuckets();

  if(strikeActive)
    return;

  // Slowly forget big waves once the sea calms down
  waveFill -= waveFill * alphaFor(WAVE_FILL_TAU);

  // Learn waves/wind/tide only while nothing looks like a fish:
  // no knock, and the bucket is neither unusually full nor filled
  // for longer than a wave can keep it filled
  bool quiet =
    !knockActive &&
    millis() - lastKnockEnd > 1000 &&
    episodeLength(pullEpisodeStart) < sustainNeed &&
    episodeLength(dropEpisodeStart) < sustainNeed &&
    pullBucket < bucketSize * 0.6 &&
    dropBucket < bucketSize * 0.6;

  if(quiet)
    updateWaveWindTide();

  updateConditions(false);
}

void finishSettling()
{
  if(pullTestAfterSettle)
  {
    pullTestWaveDeg = 0;
    startPullTest();
  }
  else
  {
    goMonitoring();
  }
}

// =====================================================
// SETTLING STEP
//
// Learns the new rest position. Knocks are still detected
// (they do not need a rest position) and a continuing pull
// is detected from local motion only.
// =====================================================

void settlingStep()
{
  unsigned long now = millis();

  bool knock = updateKnockDetector();

  updateKnockNoise();

  if(now >= settleGraceUntil && checkViolentShake())
    return;

  if(knock)
  {
    onKnock(true);

    if(strikeActive)
      return;
  }

  // ---------- Continuing pull while settling ----------

  settleRingTick++;

  if(settleRingTick >= 5)
  {
    settleRingTick = 0;

    Vec3 oldest = settleRing[settleRingIndex];

    settleRing[settleRingIndex] = gravityUnit;
    settleRingIndex = (settleRingIndex + 1) % SETTLE_RING;

    if(settleRingCount < SETTLE_RING)
    {
      settleRingCount++;
    }
    else if(now >= settleGraceUntil)
    {
      float moved = vAngleDeg(gravityUnit, oldest);

      float pullLevel = waveSwing * 2.0;

      if(pullLevel < SETTLE_PULL_MIN)
        pullLevel = SETTLE_PULL_MIN;

      if(pullLevel < settings.pullDeg * 1.5)
        pullLevel = settings.pullDeg * 1.5;

      if(moved > pullLevel)
      {
        if(!settlePullCandidate)
        {
          settlePullCandidate = true;
          settlePullStart = now;
        }

        settlePullAway =
          haveDir &&
          vDot(vSub(gravityUnit, oldest), pullDir) < 0;
      }
      else if(moved < pullLevel * 0.45)
      {
        settlePullCandidate = false;
      }
    }
  }

  if(
    settlePullCandidate &&
    now - settlePullStart >= SETTLE_PULL_TIME
  )
  {
    if(!settlePullAway || settings.dropOn > 0.5)
    {
      startStrike(settlePullAway ? "DROP-BACK" : "SLOW PULL");
      return;
    }
  }

  // ---------- Taking too long (fish still active, big waves) ----------

  if(
    now >= settleGraceUntil &&
    now - settleStart >= SETTLE_MAX_WAIT + (settleGraceUntil > settleStart ? settleGraceUntil - settleStart : 0)
  )
  {
    setRest(recentRest);

    fastFollowUntil = now + FAST_FOLLOW_MS;

    Serial.println();
    Serial.println("SETTLE TIMEOUT - USING AVERAGE POSITION");

    finishSettling();
    return;
  }

  // ---------- Learn new rest position ----------

  bool calm =
    knockEnergy < knockMean + (knockThreshold - knockMean) * 0.75 &&
    !knockActive;

  if(!calm)
  {
    // Movement restarted, do not accept yet
    resetSettleWindow();
    return;
  }

  if(!calmTimerRunning)
  {
    calmTimerRunning = true;
    calmStart = now;

    settleAnchor = gravityUnit;
    settleSum = vec(0, 0, 0);
    settleSamples = 0;
    settleMaxDev = 0;
  }

  settleSum = vAdd(settleSum, gravityUnit);
  settleSamples++;

  float dev = vAngleDeg(gravityUnit, settleAnchor);

  if(dev > settleMaxDev)
    settleMaxDev = dev;

  // Waves: wait at least one wave so they average out
  float tolerance = waveSwing * 2.5;

  if(tolerance < SETTLE_TOL_MIN)
    tolerance = SETTLE_TOL_MIN;

  unsigned long settleTime = SETTLE_TIME_MIN;

  if(waveSwing > 0.5 && wavePeriod > 0)
  {
    settleTime =
      (unsigned long)clampFloat(
        wavePeriod * 1000.0,
        SETTLE_TIME_MIN,
        SETTLE_TIME_MAX
      );
  }

  unsigned long elapsed = now - calmStart;

  float timeScore =
    clampFloat((float)elapsed / settleTime, 0, 1);

  float stabilityScore =
    clampFloat(1.0 - settleMaxDev / tolerance, 0, 1);

  settlingConfidence =
    (int)(timeScore * stabilityScore * 100.0);

  if(elapsed < settleTime)
    return;

  if(settleMaxDev >= tolerance || settleSamples == 0)
  {
    resetSettleWindow();
    return;
  }

  setRest(settleSum);

  Serial.println();
  Serial.println("NEW ROD POSITION LEARNED");

  finishSettling();
}

// =====================================================
// ONE READING
// =====================================================

void processSample()
{
  readSensor();
  updateSignals();

  switch(rodState)
  {
    case ST_LEARNING:
      learningStep();
      break;

    case ST_PULLTEST:
      pullTestStep();
      break;

    case ST_SETTLING:
      settlingStep();
      break;

    case ST_MONITORING:
      monitoringStep();
      break;

    case ST_PAUSED:
      pullBucket = 0;
      dropBucket = 0;
      settlingConfidence = 0;
      break;
  }

  updateRecording();
}

// =====================================================
// DASHBOARD
// =====================================================

const char webpage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>

<head>

<meta charset="UTF-8">

<meta name="viewport"
content="width=device-width,initial-scale=1">

<title>StrikeSense</title>

<style>

* {
  box-sizing:border-box;
}

html,
body {
  margin:0;
  width:100%;
  min-height:100%;
}

body {
  font-family:Arial,Helvetica,sans-serif;
  color:#edf5f1;

  background:
    radial-gradient(
      circle at 40% -20%,
      #153a2c 0,
      #09110d 42%,
      #030504 100%
    );

  padding:10px;
}

.app {
  width:100%;
  max-width:1550px;
  margin:auto;
}

/* HEADER */

.header {
  min-height:68px;
  display:grid;
  grid-template-columns:1fr auto 1fr;
  align-items:center;
  gap:12px;
  margin-bottom:9px;
}

.brand {
  font-size:25px;
  font-weight:900;
  letter-spacing:-.7px;
}

.brand small {
  display:block;
  color:#70847a;
  font-size:11px;
  margin-top:2px;
}

#mainStatus {
  min-width:230px;
  text-align:center;
  border-radius:13px;
  padding:10px 18px;
  background:#0a120e;
  border:1px solid #1a2a22;
  font-size:23px;
  font-weight:900;
}

.rightHeader {
  text-align:right;
  color:#74887e;
  font-size:11px;
}

.connected {
  color:#47ee8d;
}

.audioState {
  margin-top:4px;
  font-size:10px;
}

#audioStatus {
  color:#ffba52;
}

.soundControls {
  display:flex;
  justify-content:flex-end;
  flex-wrap:wrap;
  gap:4px;
  margin-top:5px;
}

.soundButton {
  border:1px solid #294036;
  background:#101a15;
  color:#c2cec8;
  border-radius:7px;
  padding:5px 7px;
  font-size:9px;
  font-weight:800;
  cursor:pointer;
}

.soundButton.enabled {
  color:#52f298;
  border-color:#3aa269;
  background:#10261a;
}

/* MAIN */

.layout {
  height:calc(100vh - 98px);
  min-height:620px;
  display:grid;
  grid-template-columns:280px minmax(400px,1fr);
  gap:9px;
}

.sidebar {
  display:grid;
  grid-template-rows:auto auto 1fr;
  gap:9px;
  min-height:0;
  overflow:auto;
}

.card {
  background:rgba(10,17,13,.95);
  border:1px solid rgba(120,180,145,.12);
  border-radius:15px;
  padding:12px;
  box-shadow:0 10px 28px rgba(0,0,0,.28);
}

.cardHead {
  display:flex;
  justify-content:space-between;
  align-items:center;
  margin-bottom:9px;
}

.cardTitle {
  color:#82968b;
  font-size:10px;
  font-weight:900;
  letter-spacing:1.5px;
  margin-bottom:9px;
}

.cardHead .cardTitle {
  margin-bottom:0;
}

.metrics {
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:6px;
}

.metric {
  background:#070d0a;
  border:1px solid #142019;
  border-radius:10px;
  padding:8px;
}

.metricLabel {
  color:#60756a;
  font-size:9px;
  margin-bottom:4px;
}

.metricValue {
  font-weight:900;
  font-size:18px;
}

.green {
  color:#4cf294 !important;
}

.blue {
  color:#59a8ff !important;
}

.orange {
  color:#ffba52 !important;
}

.red {
  color:#ff5757 !important;
}

/* CONDITIONS */

.modeBadge {
  font-size:9px;
  font-weight:900;
  letter-spacing:1px;
  padding:3px 7px;
  border-radius:6px;
  color:#52f298;
  background:#10261a;
  border:1px solid #3aa269;
}

.modeBadge.manual {
  color:#ffba52;
  background:#241a0c;
  border-color:#7a5a26;
}

.seaRow {
  display:flex;
  justify-content:space-between;
  align-items:center;
  background:#070d0a;
  border:1px solid #142019;
  border-radius:10px;
  padding:8px 10px;
}

.seaLabel {
  color:#60756a;
  font-size:10px;
  font-weight:900;
  letter-spacing:1px;
}

.seaValue {
  font-size:18px;
  font-weight:900;
}

.cond {
  margin-top:8px;
}

.condTop {
  display:flex;
  justify-content:space-between;
  align-items:baseline;
  font-size:11px;
  color:#81968a;
  margin-bottom:4px;
}

.condTop strong {
  color:#edf5f1;
  font-size:13px;
}

.condNum {
  color:#60756a;
  font-size:10px;
  margin-left:6px;
  font-weight:400;
}

.track {
  height:10px;
  border-radius:20px;
  overflow:hidden;
  background:#17211b;
}

.track.small {
  height:7px;
}

.condFill {
  width:0%;
  height:100%;
  border-radius:20px;
  background:#45ef8d;
  transition:width .5s, background .5s;
}

.seg {
  display:flex;
  gap:4px;
  margin-top:10px;
}

.segBtn {
  flex:1;
  border:1px solid #294036;
  background:#101a15;
  color:#c2cec8;
  border-radius:7px;
  padding:6px 0;
  font-size:9px;
  font-weight:900;
  cursor:pointer;
}

.segBtn.on {
  color:#001b0c;
  background:#45e98a;
  border-color:#45e98a;
}

.details {
  white-space:pre-line;
  margin-top:9px;
  color:#5d7066;
  font-size:9px;
  line-height:1.5;
  display:flex;
  justify-content:space-between;
  align-items:center;
  gap:8px;
}

/* SCORES */

.scoreBlock {
  margin-top:9px;
}

.scoreTop {
  display:flex;
  justify-content:space-between;
  font-size:11px;
  color:#81968a;
  margin-bottom:4px;
}

.scoreTop strong {
  color:white;
  font-size:16px;
}

#slowFill,
#dropFill {
  width:0%;
  height:100%;
  border-radius:20px;
  background:linear-gradient(90deg,#3e8cff,#40efa1,#ffc147,#ff5151);
}

#settleFill {
  width:0%;
  height:100%;
  border-radius:20px;
  background:linear-gradient(90deg,#ff5151,#ffba4f,#45ef8d);
}

.dirLine {
  margin-top:9px;
  font-size:10px;
  color:#74887d;
}

/* STATE */

.stateText {
  font-size:20px;
  font-weight:900;
}

.stateSub {
  color:#74887d;
  font-size:11px;
  margin-top:5px;
  line-height:1.4;
}

.controls {
  display:flex;
  gap:6px;
  flex-wrap:wrap;
  margin-top:10px;
}

button.action {
  flex:1;
  border:0;
  min-width:70px;
  padding:9px 10px;
  border-radius:9px;
  font-size:11px;
  font-weight:900;
  cursor:pointer;
}

.details button.action {
  flex:0 0 auto;
  min-width:0;
  padding:6px 9px;
  font-size:9px;
}

.relearn {
  background:#3e72ed;
  color:#fff;
}

.watch {
  background:#ebefed;
  color:#111;
}

.reel {
  background:#ee4747;
  color:#fff;
}

.ready {
  background:#45e98a;
  color:#001b0c;
}

.skip {
  background:#1d2a23;
  color:#c2cec8;
}

/* GRAPH */

.graphCard {
  min-width:0;
  min-height:0;
  display:flex;
  flex-direction:column;
}

.graphHeader {
  display:flex;
  align-items:center;
  justify-content:space-between;
  gap:10px;
  margin-bottom:5px;
  flex-wrap:wrap;
}

.graphTitle {
  font-weight:900;
  font-size:17px;
}

.graphSub {
  color:#64776e;
  font-size:10px;
}

.graphRight {
  display:flex;
  align-items:center;
  gap:12px;
  flex-wrap:wrap;
}

.legend {
  display:flex;
  gap:14px;
  color:#7d9186;
  font-size:10px;
}

.dot {
  display:inline-block;
  width:8px;
  height:8px;
  border-radius:50%;
  margin-right:4px;
}

.moveDot {
  background:#48ef91;
}

.tiltDot {
  background:#5da8ff;
}

.viewToggle {
  display:flex;
  gap:4px;
}

.soundButton:disabled {
  opacity:.4;
  cursor:default;
}

.graphNumbers {
  display:grid;
  grid-template-columns:repeat(4,1fr);
  gap:6px;
  margin:5px 0 6px;
}

.gNumber {
  padding:7px 9px;
  background:#070d0a;
  border:1px solid #142019;
  border-radius:9px;
  transition:border-color .2s, box-shadow .2s;
}

.gNumber.knockFlash {
  border-color:#4cf294;
  box-shadow:0 0 14px rgba(76,242,148,.45);
}

.gLabel {
  color:#62766b;
  font-size:8px;
}

.gValue {
  font-size:17px;
  font-weight:900;
  margin-top:2px;
}

.canvasWrap {
  flex:1;
  min-height:0;
  background:#060b08;
  border:1px solid #142019;
  border-radius:12px;
  overflow:hidden;
}

canvas {
  display:block;
  width:100%;
  height:100%;
}

/* STRIKE FLASH */

body.strikeFlash {
  animation:strikeFlashAnimation 0.65s infinite;
}

@keyframes strikeFlashAnimation {
  0% { box-shadow:inset 0 0 0 0 rgba(255,40,40,0); }
  50% { box-shadow:inset 0 0 90px 0 rgba(255,40,40,.20); }
  100% { box-shadow:inset 0 0 0 0 rgba(255,40,40,0); }
}

/* RUNNING TIME */

.uptime {
  display:flex;
  justify-content:flex-end;
  flex-wrap:wrap;
  gap:4px;
  margin-top:6px;
}

.timeChip {
  display:flex;
  align-items:baseline;
  gap:5px;
  padding:4px 8px;
  border-radius:8px;
  background:#0a120e;
  border:1px solid #1a2a22;
  font-size:9px;
  font-weight:900;
  letter-spacing:.8px;
  color:#62766b;
}

.timeChip b {
  color:#edf5f1;
  font-size:12px;
  letter-spacing:0;
  font-variant-numeric:tabular-nums;
}

.timeChip.armed b {
  color:#4cf294;
}

/* LORA / WATCH LINK */

.loraChip {
  display:flex;
  align-items:center;
  gap:6px;
  padding:4px 8px;
  border-radius:8px;
  background:#0a120e;
  border:1px solid #1a2a22;
  font-size:9px;
  font-weight:900;
  letter-spacing:.8px;
  color:#62766b;
}

.loraChip b {
  font-size:11px;
  letter-spacing:.5px;
}

.loraChip small {
  font-size:10px;
  font-weight:700;
  letter-spacing:0;
  color:#8fa398;
  font-variant-numeric:tabular-nums;
}

.bars {
  display:flex;
  align-items:flex-end;
  gap:2px;
  height:12px;
}

.bars i {
  width:3px;
  border-radius:1px;
  background:#22332a;
}

.bars i:nth-child(1) { height:4px; }
.bars i:nth-child(2) { height:6px; }
.bars i:nth-child(3) { height:9px; }
.bars i:nth-child(4) { height:12px; }

/* SETTINGS DRAWER */

.drawer {
  position:fixed;
  top:0;
  right:0;
  bottom:0;
  width:360px;
  max-width:100%;
  z-index:20;
  background:rgba(8,14,11,.97);
  border-left:1px solid rgba(120,180,145,.18);
  box-shadow:-20px 0 50px rgba(0,0,0,.5);
  padding:14px;
  overflow-y:auto;
  transform:translateX(105%);
  transition:transform .25s ease;
}

.drawer.open {
  transform:translateX(0);
}

.drawerHead {
  display:flex;
  justify-content:space-between;
  align-items:center;
  margin-bottom:10px;
}

.drawerTitle {
  font-size:17px;
  font-weight:900;
}

.closeBtn {
  border:1px solid #294036;
  background:#101a15;
  color:#c2cec8;
  border-radius:8px;
  width:32px;
  height:32px;
  font-size:16px;
  cursor:pointer;
}

.liveBox {
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:6px;
  margin-bottom:10px;
}

.liveItem {
  background:#070d0a;
  border:1px solid #142019;
  border-radius:9px;
  padding:7px 9px;
}

.liveItem .gLabel {
  margin-bottom:2px;
}

.liveItem b {
  font-size:14px;
}

.setGroup {
  color:#82968b;
  font-size:10px;
  font-weight:900;
  letter-spacing:1.5px;
  margin:14px 0 6px;
}

.setRow {
  background:#070d0a;
  border:1px solid #142019;
  border-radius:10px;
  padding:9px 10px;
  margin-bottom:6px;
}

.setTop {
  display:flex;
  justify-content:space-between;
  align-items:baseline;
  font-size:12px;
  font-weight:800;
}

.setTop strong {
  color:#4cf294;
  font-size:14px;
  font-variant-numeric:tabular-nums;
}

.setHelp {
  color:#60756a;
  font-size:10px;
  margin-top:3px;
  line-height:1.35;
}

.setRow input[type=range] {
  width:100%;
  margin:8px 0 2px;
  accent-color:#45e98a;
}

.setEnds {
  display:flex;
  justify-content:space-between;
  color:#4f6358;
  font-size:9px;
}

.switchRow {
  display:flex;
  justify-content:space-between;
  align-items:center;
}

.switch {
  border:1px solid #294036;
  background:#101a15;
  color:#c2cec8;
  border-radius:20px;
  padding:5px 12px;
  font-size:10px;
  font-weight:900;
  cursor:pointer;
  min-width:52px;
}

.switch.on {
  color:#001b0c;
  background:#45e98a;
  border-color:#45e98a;
}

.saveState {
  font-size:10px;
  color:#ffba52;
  margin:12px 0 6px;
}

.saveState.saved {
  color:#4cf294;
}

/* MOBILE */

@media(max-width:850px) {

  .layout {
    height:auto;
    display:flex;
    flex-direction:column;
  }

  .sidebar {
    display:grid;
    grid-template-columns:1fr 1fr;
    overflow:visible;
  }

  .sidebar .stateCard {
    grid-column:1 / -1;
  }

  .graphCard {
    height:460px;
  }
}

@media(max-width:600px) {

  .uptime {
    justify-content:center;
  }

  .drawer {
    top:auto;
    width:100%;
    height:62vh;
    border-left:0;
    border-top:1px solid rgba(120,180,145,.18);
    border-radius:16px 16px 0 0;
    transform:translateY(105%);
  }

  .drawer.open {
    transform:translateY(0);
  }

  .header {
    grid-template-columns:1fr;
    text-align:center;
  }

  .brand,
  .rightHeader {
    text-align:center;
  }

  .soundControls {
    justify-content:center;
  }

  .sidebar {
    display:flex;
    flex-direction:column;
  }

  .graphNumbers {
    grid-template-columns:1fr 1fr;
  }
}

</style>

</head>

<body>

<div class="app">

<!-- HEADER -->

<div class="header">

<div class="brand">
Laarif StrikeSense
<small>LIVE ROD MONITOR</small>
</div>

<div id="mainStatus" class="orange">
LEARNING
</div>

<div class="rightHeader">

<span class="connected" id="connState">&#9679; CONNECTED</span>
<br>
192.168.4.1

<div class="audioState">
AUDIO: <span id="audioStatus">OFF</span>
</div>

<div class="soundControls">
<button id="soundButton" class="soundButton" onclick="enableSound()">ENABLE SOUND</button>
<button class="soundButton" onclick="testSharpSound()">TEST SHARP</button>
<button class="soundButton" onclick="testSlowSound()">TEST SLOW</button>
<button class="soundButton" onclick="openSettings()">&#9881; SETTINGS</button>
</div>

<div class="uptime">
<span class="timeChip">&#9201; ON <b id="upTime">0:00:00</b></span>
<span class="timeChip armed">ARMED <b id="armedTime">&ndash;</b></span>
<span class="timeChip">TOTAL <b id="totalTime">0h</b></span>
<span class="loraChip" id="loraChip" title="">&#128225; WATCH
<span class="bars" id="loraBars"><i></i><i></i><i></i><i></i></span>
<b id="loraText">&ndash;</b>
<small id="loraInfo"></small>
</span>
</div>

</div>

</div>


<div class="layout">

<div class="sidebar">

<!-- CONDITIONS -->

<div class="card">

<div class="cardHead">
<div class="cardTitle">CONDITIONS</div>
<div class="modeBadge" id="modeBadge">AUTO</div>
</div>

<div class="seaRow">
<span class="seaLabel">SEA STATE</span>
<span class="seaValue orange" id="seaValue">&ndash;</span>
</div>

<div class="cond">
<div class="condTop">
<span>&#127754; WAVES</span>
<span><strong id="waveLabel">&ndash;</strong><span class="condNum" id="waveNum"></span></span>
</div>
<div class="track small"><div class="condFill" id="waveFill"></div></div>
</div>

<div class="cond">
<div class="condTop">
<span>&#128168; WIND</span>
<span><strong id="windLabel">&ndash;</strong><span class="condNum" id="windNum"></span></span>
</div>
<div class="track small"><div class="condFill" id="windFill"></div></div>
</div>

<div class="cond">
<div class="condTop">
<span>&#8600; TIDE</span>
<span><strong id="tideLabel">&ndash;</strong><span class="condNum" id="tideNum"></span></span>
</div>
<div class="track small"><div class="condFill" id="tideFill"></div></div>
</div>

<div class="seg">
<button class="segBtn" id="seg_auto" onclick="setSea('auto')">AUTO</button>
<button class="segBtn" id="seg_calm" onclick="setSea('calm')">CALM</button>
<button class="segBtn" id="seg_mod" onclick="setSea('mod')">MOD</button>
<button class="segBtn" id="seg_rough" onclick="setSea('rough')">ROUGH</button>
</div>

<div class="details">
<span id="details">&ndash;</span>
<button class="action relearn" onclick="relearn()">RELEARN</button>
</div>

</div>


<!-- DETECTION -->

<div class="card">

<div class="cardTitle">DETECTION</div>

<div class="metrics">

<div class="metric">
<div class="metricLabel">KNOCK</div>
<div class="metricValue green" id="energy">0.000</div>
</div>

<div class="metric">
<div class="metricLabel">KNOCK LIMIT</div>
<div class="metricValue" id="knockThr">0.000</div>
</div>

<div class="metric">
<div class="metricLabel">TILT &deg;</div>
<div class="metricValue blue" id="tilt">0.0</div>
</div>

<div class="metric">
<div class="metricLabel">KNOCKS (4s)</div>
<div class="metricValue" id="knockCount">0</div>
</div>

</div>

<div class="scoreBlock">
<div class="scoreTop">
<span>SLOW PULL</span>
<strong id="slowScore">0%</strong>
</div>
<div class="track"><div id="slowFill"></div></div>
</div>

<div class="scoreBlock">
<div class="scoreTop">
<span>DROP-BACK</span>
<strong id="dropScore">0%</strong>
</div>
<div class="track"><div id="dropFill"></div></div>
</div>

<div class="dirLine" id="dirLine">Direction: &ndash;</div>

</div>


<!-- STATE -->

<div class="card stateCard">

<div class="cardTitle">ROD STATE</div>

<div class="stateText" id="stateText">LEARNING</div>

<div class="stateSub" id="stateSub">Learning environment.</div>

<div class="scoreBlock">
<div class="scoreTop">
<span>SETTLING CONFIDENCE</span>
<strong id="settleScore">0%</strong>
</div>
<div class="track"><div id="settleFill"></div></div>
</div>

<div class="controls">
<button id="watchButton" class="action watch" onclick="watching()" style="display:none">I'M WATCHING</button>
<button id="reelButton" class="action reel" onclick="reelIn()" style="display:none">REEL IN</button>
<button id="readyButton" class="action ready" onclick="ready()" style="display:none">READY</button>
<button id="skipButton" class="action skip" onclick="skipPull()" style="display:none">SKIP</button>
</div>

</div>

</div>


<!-- GRAPH -->

<div class="card graphCard">

<div class="graphHeader">

<div>
<div class="graphTitle" id="graphTitle">LIVE ROD ACTIVITY</div>
<div class="graphSub" id="graphSub">Top: knock energy &middot; Bottom: tilt (+ toward water)</div>
</div>

<div class="graphRight">

<div class="legend">
<span><span class="dot moveDot"></span>Knock</span>
<span><span class="dot tiltDot"></span>Tilt</span>
</div>

<div class="viewToggle">
<button class="soundButton enabled" id="viewLive" onclick="setView('live')">LIVE</button>
<button class="soundButton" id="viewStrike" onclick="setView('strike')" disabled>LAST STRIKE</button>
<button class="soundButton" id="view3d" onclick="setView('3d')">3D NOISE</button>
</div>

</div>

</div>

<div class="graphNumbers">

<div class="gNumber" id="knockBox">
<div class="gLabel">KNOCK</div>
<div class="gValue green" id="graphEnergy">0.000</div>
</div>

<div class="gNumber">
<div class="gLabel">TILT &deg;</div>
<div class="gValue blue" id="graphTilt">0.0</div>
</div>

<div class="gNumber">
<div class="gLabel">SLOW PULL</div>
<div class="gValue" id="graphSlow">0%</div>
</div>

<div class="gNumber">
<div class="gLabel">LAST STRIKE</div>
<div class="gValue" id="lastStrike">NONE</div>
</div>

</div>

<div class="canvasWrap">
<canvas id="graph"></canvas>
</div>

</div>

</div>

</div>


<script>

// =====================================================
// HELPER
// =====================================================

function el(id)
{
  return document.getElementById(id);
}

// =====================================================
// AUDIO
// =====================================================

let soundEnabled = false;
let audioContext = null;
let lastPlayedStrikeID = 0;
let alarmTimer = null;
let activeAlarmType = "";
let wakeLock = null;

// WAV fallback for browsers without Web Audio

function makeToneWav(frequency, durationMs)
{
  const sampleRate = 8000;
  const samples = Math.floor(sampleRate * durationMs / 1000);
  const buffer = new ArrayBuffer(44 + samples * 2);
  const view = new DataView(buffer);

  function writeString(offset, text)
  {
    for(let i = 0; i < text.length; i++)
      view.setUint8(offset + i, text.charCodeAt(i));
  }

  writeString(0, "RIFF");
  view.setUint32(4, 36 + samples * 2, true);
  writeString(8, "WAVE");
  writeString(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeString(36, "data");
  view.setUint32(40, samples * 2, true);

  const amplitude = 12000;

  for(let i = 0; i < samples; i++)
  {
    const phase = (i * frequency / sampleRate) % 1;
    view.setInt16(44 + i * 2, phase < 0.5 ? amplitude : -amplitude, true);
  }

  const bytes = new Uint8Array(buffer);
  let binary = "";

  for(let i = 0; i < bytes.length; i++)
    binary += String.fromCharCode(bytes[i]);

  return "data:audio/wav;base64," + btoa(binary);
}

function fallbackTone(frequency, durationMs, delayMs)
{
  setTimeout(() =>
  {
    try
    {
      const sound = new Audio(makeToneWav(frequency, durationMs));
      sound.volume = 1.0;
      sound.play().catch(() => {});
    }
    catch(e)
    {
    }
  }, delayMs);
}

function webAudioTone(frequency, durationMs, delayMs)
{
  const start = audioContext.currentTime + delayMs / 1000;
  const duration = durationMs / 1000;

  const oscillator = audioContext.createOscillator();
  const gain = audioContext.createGain();

  oscillator.type = "square";
  oscillator.frequency.setValueAtTime(frequency, start);

  gain.gain.setValueAtTime(0.001, start);
  gain.gain.linearRampToValueAtTime(0.30, start + 0.015);
  gain.gain.setValueAtTime(0.30, start + Math.max(0.02, duration - 0.04));
  gain.gain.linearRampToValueAtTime(0.001, start + duration);

  oscillator.connect(gain);
  gain.connect(audioContext.destination);

  oscillator.start(start);
  oscillator.stop(start + duration + 0.03);
}

function tone(frequency, duration, delay)
{
  if(!soundEnabled)
    return;

  if(audioContext && audioContext.state === "running")
    webAudioTone(frequency, duration, delay);
  else
    fallbackTone(frequency, duration, delay);
}

// Keep the phone screen on while the dashboard is open
async function keepScreenOn()
{
  try
  {
    if("wakeLock" in navigator)
      wakeLock = await navigator.wakeLock.request("screen");
  }
  catch(e)
  {
  }
}

document.addEventListener("visibilitychange", () =>
{
  if(document.visibilityState === "visible" && soundEnabled)
    keepScreenOn();
});

async function enableSound()
{
  try
  {
    if(!audioContext)
    {
      const AudioClass = window.AudioContext || window.webkitAudioContext;

      if(AudioClass)
        audioContext = new AudioClass();
    }

    if(audioContext)
    {
      try
      {
        await audioContext.resume();
      }
      catch(e)
      {
      }
    }

    soundEnabled = true;

    el("audioStatus").innerText = "READY";
    el("audioStatus").style.color = "#52f298";
    el("soundButton").innerText = "SOUND ON";
    el("soundButton").classList.add("enabled");

    keepScreenOn();

    tone(900, 180, 0);
  }
  catch(e)
  {
    soundEnabled = true;
    el("audioStatus").innerText = "FALLBACK";
    fallbackTone(900, 300, 0);
  }
}

// =====================================================
// ALARM PATTERNS
// =====================================================

function vibrate(pattern)
{
  if(navigator.vibrate)
    navigator.vibrate(pattern);
}

function sharpPattern()
{
  tone(1350, 110, 0);
  tone(1750, 110, 150);
  tone(1350, 110, 300);
  tone(1850, 180, 450);
  vibrate([100, 60, 100, 60, 180]);
}

function slowPattern()
{
  tone(430, 500, 0);
  tone(600, 600, 650);
  vibrate([450, 200, 550]);
}

function repeatedPattern()
{
  tone(850, 140, 0);
  tone(850, 140, 200);
  tone(1050, 180, 400);
  vibrate([100, 90, 100, 90, 160]);
}

function dropPattern()
{
  tone(900, 300, 0);
  tone(650, 300, 380);
  tone(450, 450, 760);
  vibrate([300, 80, 300, 80, 450]);
}

// Single knock: one short beep, no alarm
function knockPattern()
{
  tone(1250, 70, 0);
  vibrate(40);
}

function playAlarmPattern()
{
  if(!soundEnabled || activeAlarmType === "")
    return;

  if(activeAlarmType === "SHARP STRIKE") sharpPattern();
  else if(activeAlarmType === "SLOW PULL") slowPattern();
  else if(activeAlarmType === "REPEATED BITE") repeatedPattern();
  else if(activeAlarmType === "DROP-BACK") dropPattern();
}

function startContinuousAlarm(type)
{
  document.body.classList.add("strikeFlash");

  if(!soundEnabled)
    return;

  if(activeAlarmType === type && alarmTimer !== null)
    return;

  stopContinuousAlarm();

  document.body.classList.add("strikeFlash");

  activeAlarmType = type;

  playAlarmPattern();

  let repeatTime = 1500;

  if(type === "SHARP STRIKE") repeatTime = 1200;
  else if(type === "SLOW PULL") repeatTime = 1800;
  else if(type === "REPEATED BITE") repeatTime = 1400;
  else if(type === "DROP-BACK") repeatTime = 1900;

  alarmTimer = setInterval(playAlarmPattern, repeatTime);

  el("audioStatus").innerText = "ALARM";
}

function stopContinuousAlarm()
{
  const wasRinging = alarmTimer !== null;

  if(wasRinging)
  {
    clearInterval(alarmTimer);
    alarmTimer = null;
  }

  activeAlarmType = "";

  document.body.classList.remove("strikeFlash");

  if(wasRinging)
    vibrate(0);

  if(soundEnabled)
    el("audioStatus").innerText = "READY";
}

async function testSharpSound()
{
  if(!soundEnabled)
    await enableSound();

  sharpPattern();
}

async function testSlowSound()
{
  if(!soundEnabled)
    await enableSound();

  slowPattern();
}

// =====================================================
// GRAPH
// =====================================================

const canvas = el("graph");
const ctx = canvas.getContext("2d");

const maxPoints = 300;     // about 30 s at 10 updates per second

let liveEnergy = [];
let liveThr = [];
let liveTilt = [];
let liveAllow = [];

let view = "live";
let replay = null;
let replayID = -1;

function resizeCanvas()
{
  const box = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;

  canvas.width = Math.floor(box.width * dpr);
  canvas.height = Math.floor(box.height * dpr);

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

  redraw();
}

window.addEventListener("resize", resizeCanvas);

// =====================================================
// 3D NOISE VIEW
// =====================================================

const NOISE_POINTS = 300;        // 3 s at 100 per second

let noiseCloud = [];             // [x, y, z] in g
let noiseDown = [0, 0, -1];      // gravity direction, sensor frame
let noiseThr = 0.01;
let noiseSensor = "";
let noiseRange = 0.02;           // g at the edge of the view (auto)
let yaw = 0.7;
let pitch = 0.45;
let dragging = false;
let dragX = 0;
let dragY = 0;
let lastDrag = 0;

function addNoisePoints(d)
{
  if(!d.vib)
    return;

  for(let i = 0; i + 2 < d.vib.length; i += 3)
    noiseCloud.push([d.vib[i] / 10000, d.vib[i + 1] / 10000, d.vib[i + 2] / 10000]);

  if(noiseCloud.length > NOISE_POINTS)
    noiseCloud.splice(0, noiseCloud.length - NOISE_POINTS);

  if(d.g)
    noiseDown = [-d.g[0], -d.g[1], -d.g[2]];

  noiseThr = d.knockthr;
  noiseSensor = d.sname || "";
}

function noiseRms()
{
  if(noiseCloud.length === 0)
    return 0;

  let sum = 0;

  for(const p of noiseCloud)
    sum += p[0] * p[0] + p[1] * p[1] + p[2] * p[2];

  return Math.sqrt(sum / noiseCloud.length);
}

// Typical size of the shaking (median): one bump does not change it
function noiseTypical()
{
  if(noiseCloud.length === 0)
    return 0;

  const m = noiseCloud.map(p => Math.sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2])).sort((a, b) => a - b);

  return m[Math.floor(m.length / 2)];
}

function noisePeak()
{
  let peak = 0;

  for(const p of noiseCloud)
    peak = Math.max(peak, Math.sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));

  return peak;
}

function mg(g)
{
  const v = g * 1000;
  return (v < 10 ? v.toFixed(1) : v.toFixed(0)) + " mg";
}

function drawNoise3D()
{
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;

  ctx.clearRect(0, 0, w, h);

  const bg = ctx.createRadialGradient(w / 2, h / 2, 10, w / 2, h / 2, Math.max(w, h) * 0.7);
  bg.addColorStop(0, "#0d1a13");
  bg.addColorStop(1, "#040806");
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, w, h);

  const rms = noiseTypical();
  const peak = noisePeak();

  // Zoom follows the noise and the knock limit, smoothly
  const target = Math.max(noiseThr * 1.3, rms * 4, 0.001);
  noiseRange += (target - noiseRange) * 0.06;

  if(!dragging && Date.now() - lastDrag > 4000)
    yaw += 0.004;

  const cx = w / 2;
  const cy = h / 2 + 10;
  const size = Math.min(w, h) * 0.38;
  const cyw = Math.cos(yaw), syw = Math.sin(yaw);
  const cp = Math.cos(pitch), sp = Math.sin(pitch);

  // p in "view units" (1 = edge of the view)
  function proj(p)
  {
    const x = p[0] * cyw - p[1] * syw;
    const y = p[0] * syw + p[1] * cyw;
    const z = p[2];

    const y2 = y * cp - z * sp;
    const z2 = y * sp + z * cp;

    const k = 4 / (4 + y2);

    return [cx + x * size * k, cy - z2 * size * k, y2];
  }

  function lineP(a, b, colour, width, dash)
  {
    const pa = proj(a), pb = proj(b);

    ctx.beginPath();
    ctx.moveTo(pa[0], pa[1]);
    ctx.lineTo(pb[0], pb[1]);
    ctx.setLineDash(dash || []);
    ctx.strokeStyle = colour;
    ctx.lineWidth = width;
    ctx.stroke();
    ctx.setLineDash([]);
  }

  function ring(r, plane, colour, width, dash)
  {
    ctx.beginPath();

    for(let i = 0; i <= 48; i++)
    {
      const t = i / 48 * Math.PI * 2;
      const a = r * Math.cos(t), b = r * Math.sin(t);
      const p = plane === 0 ? [a, b, 0] : plane === 1 ? [a, 0, b] : [0, a, b];
      const q = proj(p);

      if(i === 0) ctx.moveTo(q[0], q[1]);
      else ctx.lineTo(q[0], q[1]);
    }

    ctx.setLineDash(dash || []);
    ctx.strokeStyle = colour;
    ctx.lineWidth = width;
    ctx.stroke();
    ctx.setLineDash([]);
  }

  // Floor rings (scale), axes
  ring(0.5, 0, "rgba(255,255,255,.06)", 1);
  ring(1.0, 0, "rgba(255,255,255,.09)", 1);

  const axes = [[[1, 0, 0], "#ff6b6b", "X"], [[0, 1, 0], "#4cf294", "Y"], [[0, 0, 1], "#59a8ff", "Z"]];

  ctx.font = "bold 11px Arial";

  for(const a of axes)
  {
    lineP([0, 0, 0], a[0], a[1], 1.2);
    lineP([0, 0, 0], a[0].map(v => -v), "rgba(255,255,255,.08)", 1);

    const q = proj(a[0].map(v => v * 1.08));
    ctx.fillStyle = a[1];
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(a[2], q[0], q[1]);
  }

  // Which way is down
  const dn = noiseDown.map(v => v * 0.85);
  lineP([0, 0, 0], dn, "rgba(200,210,205,.35)", 2, [3, 4]);
  const qd = proj(dn);
  ctx.fillStyle = "rgba(200,210,205,.6)";
  ctx.fillText("down", qd[0], qd[1] + 10);

  // Knock limit sphere
  const rt = noiseThr / noiseRange;

  for(let pl = 0; pl < 3; pl++)
    ring(rt, pl, "rgba(255,186,82,.55)", 1.3, [5, 4]);

  // Points: older = fainter; outside the knock limit = orange
  const n = noiseCloud.length;

  for(let i = 0; i < n; i++)
  {
    const p = noiseCloud[i];
    const len = Math.sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    const q = proj(p.map(v => v / noiseRange));
    const age = i / Math.max(1, n - 1);
    const out = len > noiseThr;

    ctx.globalAlpha = 0.15 + 0.85 * age;
    ctx.fillStyle = out ? "#ffba52" : "#49ef92";
    ctx.beginPath();
    ctx.arc(q[0], q[1], i === n - 1 ? 3.5 : 2.2, 0, Math.PI * 2);
    ctx.fill();
  }

  ctx.globalAlpha = 1;

  // Numbers
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.font = "bold 13px Arial";
  ctx.fillStyle = "#49ef92";
  ctx.fillText("NOISE  " + mg(rms) + " typical", 12, 10);
  ctx.fillStyle = "#ffba52";
  ctx.fillText("KNOCK LIMIT  " + mg(noiseThr), 12, 28);
  ctx.fillStyle = "#7d9186";
  ctx.font = "11px Arial";
  ctx.fillText("limit is " + (rms > 0 ? (noiseThr / rms).toFixed(1) : "-") + "x the noise · peak " + mg(peak) + (noiseSensor ? " · " + noiseSensor : ""), 12, 46);
  ctx.textBaseline = "bottom";
  ctx.fillText("axis length = " + mg(noiseRange) + " · zoom follows the noise", 12, h - 8);
}

function noiseLoop()
{
  if(view === "3d")
    drawNoise3D();

  requestAnimationFrame(noiseLoop);
}

canvas.addEventListener("pointerdown", e =>
{
  if(view !== "3d")
    return;

  dragging = true;
  dragX = e.clientX;
  dragY = e.clientY;
  canvas.setPointerCapture(e.pointerId);
});

canvas.addEventListener("pointermove", e =>
{
  if(!dragging)
    return;

  yaw += (e.clientX - dragX) * 0.01;
  pitch = Math.max(-1.4, Math.min(1.4, pitch + (e.clientY - dragY) * 0.01));
  dragX = e.clientX;
  dragY = e.clientY;
  lastDrag = Date.now();
});

canvas.addEventListener("pointerup", () =>
{
  dragging = false;
  lastDrag = Date.now();
});

function redraw()
{
  if(view === "3d")
  {
    drawNoise3D();
    return;
  }

  if(view === "strike" && replay)
  {
    const n = replay.e.length;

    drawLanes(
      replay.e,
      new Array(n).fill(replay.thr),
      replay.t,
      new Array(n).fill(replay.allow),
      n,
      replay.pre,
      replay.dir
    );
  }
  else
  {
    drawLanes(liveEnergy, liveThr, liveTilt, liveAllow, maxPoints, -1, lastDir);
  }
}

function drawLanes(E, THR, T, AL, total, marker, signed)
{
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;

  ctx.clearRect(0, 0, width, height);

  const bg = ctx.createLinearGradient(0, 0, 0, height);
  bg.addColorStop(0, "#0b1510");
  bg.addColorStop(1, "#050906");
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, width, height);

  const left = 48;
  const right = 10;
  const top = 10;
  const bottom = 12;
  const gap = 16;

  const gw = width - left - right;
  const laneH = (height - top - bottom - gap) / 2;

  const lane1Top = top;
  const lane2Top = top + laneH + gap;

  const n = E.length;
  const offset = total - n;

  function xAt(i)
  {
    return left + gw * (i + offset) / Math.max(1, total - 1);
  }

  // ---------- Scales ----------

  let maxE = 0.02;

  for(const v of E) if(v > maxE) maxE = v;
  for(const v of THR) if(v * 1.6 > maxE) maxE = v * 1.6;

  maxE *= 1.12;

  let maxT = 1.0;

  for(const v of T) if(Math.abs(v) > maxT) maxT = Math.abs(v);
  for(const v of AL) if(v * 1.6 > maxT) maxT = v * 1.6;

  maxT *= 1.12;

  const minT = signed ? -maxT : 0;

  function yE(v)
  {
    return lane1Top + laneH - Math.min(v / maxE, 1) * laneH;
  }

  function yT(v)
  {
    const r = (Math.max(minT, Math.min(maxT, v)) - minT) / (maxT - minT);
    return lane2Top + laneH - r * laneH;
  }

  // ---------- Grid + labels ----------

  ctx.font = "10px Arial";
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";

  for(let i = 0; i <= 4; i++)
  {
    const v = maxE * (1 - i / 4);
    const y = yE(v);

    ctx.strokeStyle = "rgba(255,255,255,.055)";
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(left + gw, y);
    ctx.stroke();

    ctx.fillStyle = "#52665b";
    ctx.fillText(v.toFixed(3), left - 6, y);
  }

  for(let i = 0; i <= 4; i++)
  {
    const v = maxT - (maxT - minT) * i / 4;
    const y = yT(v);

    ctx.strokeStyle = Math.abs(v) < 0.0001 ? "rgba(255,255,255,.16)" : "rgba(255,255,255,.055)";
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(left + gw, y);
    ctx.stroke();

    ctx.fillStyle = "#52665b";
    ctx.fillText(v.toFixed(1) + "°", left - 6, y);
  }

  for(let i = 0; i <= 8; i++)
  {
    const x = left + gw * i / 8;

    ctx.strokeStyle = "rgba(255,255,255,.035)";
    ctx.beginPath();
    ctx.moveTo(x, lane1Top);
    ctx.lineTo(x, lane1Top + laneH);
    ctx.moveTo(x, lane2Top);
    ctx.lineTo(x, lane2Top + laneH);
    ctx.stroke();
  }

  ctx.textAlign = "left";
  ctx.fillStyle = "#4f6358";
  ctx.fillText("KNOCK ENERGY (g)", left + 6, lane1Top + 9);
  ctx.fillText(signed ? "TILT (+ toward water / - away)" : "TILT (amount, direction not set)", left + 6, lane2Top + 9);

  // ---------- Series ----------

  function line(data, yFn, colour, width, dash)
  {
    if(data.length < 2)
      return;

    ctx.beginPath();
    ctx.moveTo(xAt(0), yFn(data[0]));

    for(let i = 1; i < data.length; i++)
      ctx.lineTo(xAt(i), yFn(data[i]));

    ctx.setLineDash(dash || []);
    ctx.strokeStyle = colour;
    ctx.lineWidth = width;
    ctx.stroke();
    ctx.setLineDash([]);
  }

  function area(data, yFn, baseY, fillColour)
  {
    if(data.length < 2)
      return;

    ctx.beginPath();
    ctx.moveTo(xAt(0), baseY);

    for(let i = 0; i < data.length; i++)
      ctx.lineTo(xAt(i), yFn(data[i]));

    ctx.lineTo(xAt(data.length - 1), baseY);
    ctx.closePath();
    ctx.fillStyle = fillColour;
    ctx.fill();
  }

  area(E, yE, yE(0), "rgba(73,239,146,.13)");
  line(E, yE, "#49ef92", 1.6);
  line(THR, yE, "rgba(255,186,82,.75)", 1, [5, 4]);

  area(T, yT, yT(0), "rgba(89,168,255,.12)");
  line(T, yT, "#59a8ff", 1.8);
  line(AL, yT, "rgba(255,186,82,.75)", 1, [5, 4]);

  if(signed)
    line(AL.map(v => -v), yT, "rgba(255,186,82,.45)", 1, [5, 4]);

  // ---------- Strike marker ----------

  if(marker >= 0)
  {
    const x = xAt(marker);

    ctx.strokeStyle = "#ff5757";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(x, lane1Top);
    ctx.lineTo(x, lane2Top + laneH);
    ctx.stroke();

    ctx.fillStyle = "#ff5757";
    ctx.textAlign = "left";
    ctx.fillText("STRIKE", x + 4, lane1Top + 22);

    ctx.fillStyle = "#52665b";
    ctx.textAlign = "center";
    ctx.fillText("-4 s", left + 14, lane2Top + laneH + 6);
    ctx.fillText("+4 s", left + gw - 14, lane2Top + laneH + 6);
  }
}

// =====================================================
// GRAPH VIEW
// =====================================================

async function loadReplay(id)
{
  try
  {
    const response = await fetch("/strike");
    replay = await response.json();
    replayID = id;
  }
  catch(e)
  {
  }
}

async function setView(v)
{
  view = v;

  el("viewLive").classList.toggle("enabled", v === "live");
  el("viewStrike").classList.toggle("enabled", v === "strike");
  el("view3d").classList.toggle("enabled", v === "3d");

  canvas.style.touchAction = v === "3d" ? "none" : "auto";

  if(v === "strike")
  {
    if(replayID !== lastStrikeID)
      await loadReplay(lastStrikeID);

    el("graphTitle").innerText = "LAST STRIKE REPLAY";
    el("graphSub").innerText = replay ? replay.type + " · 4 s before and after" : "No strike recorded yet";
  }
  else if(v === "3d")
  {
    el("graphTitle").innerText = "3D NOISE";
    el("graphSub").innerText = "Each dot = how the tip shook (100 per second, last 3 s) · orange = knock limit · drag to turn";
  }
  else
  {
    el("graphTitle").innerText = "LIVE ROD ACTIVITY";
    el("graphSub").innerText = "Top: knock energy · Bottom: tilt (+ toward water)";
  }

  redraw();
}

// =====================================================
// BUTTONS
// =====================================================

async function relearn()
{
  stopContinuousAlarm();
  await fetch("/relearn");
}

async function watching()
{
  stopContinuousAlarm();
  await fetch("/watching");
}

async function reelIn()
{
  stopContinuousAlarm();
  await fetch("/reelin");
}

async function ready()
{
  stopContinuousAlarm();
  await fetch("/ready");
}

async function skipPull()
{
  await fetch("/skippull");
}

// =====================================================
// LIVE SETTINGS
// =====================================================

let settings = null;
let slideTimer = null;

function showSettings(st)
{
  settings = st;

  for(const k of ["kmul", "pdeg", "ptime", "rcnt", "rwin", "sharp"])
    el("s_" + k).value = st[k];

  el("v_kmul").innerText = "\u00D7" + st.kmul.toFixed(2);
  el("v_rep").innerText = Math.round(st.rcnt) + " in " + st.rwin.toFixed(1) + " s";
  el("v_sharp").innerText = st.sharp.toFixed(2) + " g (~" + (st.sharp / 0.3).toFixed(2) + " g hit)";
  el("v_pdeg").innerText = st.pdeg.toFixed(1) + "\u00B0";
  el("v_ptime").innerText = st.ptime.toFixed(1) + " s";

  for(const k of ["kbeep", "drop"])
  {
    el("t_" + k).classList.toggle("on", st[k] > 0.5);
    el("t_" + k).innerText = st[k] > 0.5 ? "ON" : "OFF";
  }

  el("saveState").innerText = st.saved
    ? "\u2713 Saved \u2013 kept after power off"
    : "Changed \u2013 press SAVE to keep after power off";
  el("saveState").classList.toggle("saved", st.saved);
}

async function settingsCall(url)
{
  try
  {
    const response = await fetch(url);
    showSettings(await response.json());
  }
  catch(e)
  {
  }
}

function openSettings()
{
  el("drawer").classList.add("open");
  settingsCall("/settings");
}

function closeSettings()
{
  el("drawer").classList.remove("open");
}

// Send while sliding, but not more than ~6 times a second
function slide(k)
{
  const v = parseFloat(el("s_" + k).value);

  if(settings)
  {
    settings[k] = v;
    settings.saved = false;
    showSettings(settings);
  }

  clearTimeout(slideTimer);
  slideTimer = setTimeout(() => settingsCall("/set?k=" + k + "&v=" + v), 150);
}

function toggle(k)
{
  const v = settings && settings[k] > 0.5 ? 0 : 1;
  settingsCall("/set?k=" + k + "&v=" + v);
}

function saveSettings()
{
  settingsCall("/savesettings");
}

function defaultSettings()
{
  settingsCall("/defaultsettings");
}

function showLiveInDrawer(d)
{
  if(!el("drawer").classList.contains("open"))
    return;

  el("lvKnock").innerText = d.energy.toFixed(3) + " / " + d.knockthr.toFixed(3);
  el("lvTilt").innerText = (d.tilt >= 0 && d.dir ? "+" : "") + d.tilt.toFixed(1) + "\u00B0 / " + d.allow.toFixed(1) + "\u00B0";
  el("lvSlow").innerText = Math.max(d.bucket, d.drop) + "%";
  el("lvKnocks").innerText = d.knocks;
}

// =====================================================
// RUNNING TIME
// =====================================================

function clock(sec)
{
  const h = Math.floor(sec / 3600);
  const m = Math.floor(sec / 60) % 60;
  const s = sec % 60;

  return h + ":" + String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0");
}

function hours(sec)
{
  const h = sec / 3600;

  return h < 10 ? h.toFixed(1) + "h" : Math.round(h) + "h";
}

function showTimes(d)
{
  el("upTime").innerText = clock(d.up);
  el("armedTime").innerText = d.armed >= 0 ? clock(d.armed) : "\u2013";
  el("totalTime").innerText = hours(d.total);
}

const LORA_NAMES = ["", "POOR", "WEAK", "FAIR", "GOOD"];
const LORA_COLOURS = ["#ff5757", "#ff5757", "#ffba52", "#c8e86a", "#45ef8d"];

function showLora(d)
{
  const bars = el("loraBars").children;
  let text, colour, info = "", tip = "";
  let level = 0;

  if(d.lora === undefined || d.lora === false)
  {
    text = "LORA OFF";
    colour = "#62766b";
    tip = "The rod's LoRa radio is not working";
  }
  else if(!d.watch)
  {
    text = "NOT CONNECTED";
    colour = "#ff5757";
    tip = d.wseen >= 0 ? "Watch last heard " + d.wseen + " s ago" : "Watch never heard since power on";
  }
  else
  {
    level = d.wbars;
    text = "CONNECTED \u00b7 " + LORA_NAMES[level];
    colour = LORA_COLOURS[level];
    info = d.wrssi + " dBm";
    tip = "Rod hears watch: " + d.wrssi + " dBm, SNR " + d.wsnr + " dB";

    if(d.wrep)
      tip += "\nWatch hears rod: " + d.w2rssi + " dBm, SNR " + d.w2snr + " dB";
  }

  for(let i = 0; i < 4; i++)
    bars[i].style.background = i < level ? colour : "#22332a";

  el("loraText").innerText = text;
  el("loraText").style.color = colour;
  el("loraInfo").innerText = info;
  el("loraChip").title = tip;
}

async function setSea(mode)
{
  await fetch("/sea?m=" + mode);
}

// =====================================================
// LIVE UPDATE
// =====================================================

const WAVE_NAMES = ["Flat", "Small", "Medium", "Big"];
const WIND_NAMES = ["Calm", "Normal", "Strong", "Very strong"];
const TIDE_NAMES = ["Steady", "Slow drift", "Fast drift"];
const SEA_NAMES = ["CALM", "MODERATE", "ROUGH"];
const SEA_CLASS = ["green", "orange", "red"];
const SEA_KEYS = ["calm", "mod", "rough"];
const LEVEL_COLOURS = ["#45ef8d", "#45ef8d", "#ffba52", "#ff5757"];

let lastKnockID = -1;
let lastStrikeID = 0;
let lastDir = false;

function setCondition(name, label, num, pct, level)
{
  el(name + "Label").innerText = label;
  el(name + "Num").innerText = num;
  el(name + "Fill").style.width = Math.max(4, Math.min(100, pct)) + "%";
  el(name + "Fill").style.background = LEVEL_COLOURS[level];
}

function showConditions(d)
{
  if(d.state === "learning")
  {
    el("seaValue").innerText = "LEARNING";
    el("seaValue").className = "seaValue orange";

    for(const n of ["wave", "wind", "tide"])
    {
      el(n + "Label").innerText = "…";
      el(n + "Num").innerText = "";
      el(n + "Fill").style.width = "0%";
    }

    el("details").innerText = "Keep the rod still";
    return;
  }

  el("seaValue").innerText = "● " + SEA_NAMES[d.sea];
  el("seaValue").className = "seaValue " + SEA_CLASS[d.sea];

  el("modeBadge").innerText = d.seaauto ? "AUTO" : "MANUAL";
  el("modeBadge").classList.toggle("manual", !d.seaauto);

  for(const k of ["auto", "calm", "mod", "rough"])
    el("seg_" + k).classList.remove("on");

  el("seg_" + (d.seaauto ? "auto" : SEA_KEYS[d.sea])).classList.add("on");

  let waveNum = "±" + d.waveswing.toFixed(1) + "°";

  if(d.period > 0)
    waveNum += " · " + d.period.toFixed(0) + " s";

  setCondition("wave", WAVE_NAMES[d.wave], waveNum, d.waveswing / 4.0 * 100, d.wave);

  const windNum = d.dir
    ? "±" + d.windswing.toFixed(1) + "°"
    : d.windvib.toFixed(3) + " g";

  const windPct = d.dir
    ? Math.max(d.windswing / 4.0, d.windvib / 0.03) * 100
    : d.windvib / 0.03 * 100;

  setCondition("wind", WIND_NAMES[d.wind], windNum, windPct, d.wind);

  setCondition("tide", TIDE_NAMES[d.tide], d.tiderate.toFixed(1) + "°/min", d.tiderate / 3.0 * 100, d.tide);

  el("details").innerText =
    "knock limit " + d.knockthr.toFixed(3) + " g · pull allow " + d.allow.toFixed(1) + "°" +
    "\nsensor noise " + d.noise.toFixed(3) + " g · " + d.sname + (d.hr ? " ✓" : " high-res OFF") + (d.serr > 0 ? " · " + d.serr + " read errors" : "");
}

function showButtons(d)
{
  el("watchButton").style.display = "none";
  el("reelButton").style.display = "none";
  el("readyButton").style.display = "none";
  el("skipButton").style.display = "none";

  if(d.strike)
  {
    el("watchButton").style.display = "block";
    el("reelButton").style.display = "block";
  }
  else if(d.state === "paused")
  {
    el("readyButton").style.display = "block";
  }
  else if(d.state === "pulltest")
  {
    el("skipButton").style.display = "block";
  }
}

function setStatus(main, cls, state, sub)
{
  el("mainStatus").innerText = main;
  el("mainStatus").className = cls;
  el("stateText").innerText = state;
  el("stateSub").innerText = sub;
}

function showState(d)
{
  if(d.strike)
  {
    setStatus(
      "STRIKE DETECTED", "red", d.type,
      "Peak knock " + d.peakenergy.toFixed(3) + " g | Peak tilt " + d.peaktilt.toFixed(1) +
      "° | Alarm continues until acknowledged."
    );
  }
  else if(d.state === "paused")
  {
    setStatus("DETECTION PAUSED", "orange", "REEL IN MODE", "Move or reel freely. Detection is disabled.");
  }
  else if(d.state === "learning")
  {
    setStatus("LEARNING", "orange", "LEARNING ENVIRONMENT", d.timeleft + " seconds remaining. Keep the rod still.");
  }
  else if(d.state === "pulltest")
  {
    setStatus(
      "PULL TEST", "orange", "PULL THE LINE GENTLY",
      "Pull the line toward the water once, then let go. Tip moved " + d.ptdeg.toFixed(1) +
      "° (need " + d.ptneed.toFixed(1) + "°)."
    );
  }
  else if(d.state === "settling")
  {
    setStatus(
      "WATCHING + SETTLING", "orange", "LEARNING NEW POSITION",
      d.grace
        ? "Place the rod. Knocks are ignored for a few seconds."
        : "Waiting for the rod to become stable. Knocks are still detected."
    );
  }
  else
  {
    let sub = "Watching knocks, slow pulls and drop-backs.";

    if(d.dirage >= 0 && d.dirage < 6)
      sub = "Direction learned ✓ Rod armed.";
    else if(d.bucket > 60)
      sub = "Possible slow pull building up…";
    else if(d.drop > 60)
      sub = "Possible drop-back building up…";
    else if(d.knocks > 0)
      sub = d.knocks + " knock(s) in the last 4 s…";

    setStatus("MONITORING", "green", "ROD ARMED", sub);
  }
}

function flashKnock()
{
  el("knockBox").classList.add("knockFlash");
  setTimeout(() => el("knockBox").classList.remove("knockFlash"), 350);
}

async function updateData()
{
  try
  {
    const response = await fetch("/data");
    const d = await response.json();

    el("connState").innerHTML = "&#9679; CONNECTED";
    el("connState").className = "connected";

    lastDir = d.dir;

    el("energy").innerText = d.energy.toFixed(3);
    el("knockThr").innerText = d.knockthr.toFixed(3);
    el("tilt").innerText = (d.tilt >= 0 && d.dir ? "+" : "") + d.tilt.toFixed(1);
    el("knockCount").innerText = d.knocks;

    el("slowScore").innerText = d.bucket + "%";
    el("slowFill").style.width = d.bucket + "%";
    el("dropScore").innerText = d.dir ? d.drop + "%" : "OFF";
    el("dropFill").style.width = d.drop + "%";

    el("settleScore").innerText = d.confidence + "%";
    el("settleFill").style.width = d.confidence + "%";

    el("dirLine").innerText = d.dir
      ? "Direction: learned ✓ (+ = toward water)"
      : "Direction: not set – do a pull test after READY";

    el("graphEnergy").innerText = d.energy.toFixed(3);
    el("graphTilt").innerText = d.tilt.toFixed(1);
    el("graphSlow").innerText = Math.max(d.bucket, d.drop) + "%";
    el("lastStrike").innerText = d.lasttype;

    showConditions(d);
    showState(d);
    showButtons(d);
    showTimes(d);
    showLora(d);
    showLiveInDrawer(d);

    // ---------- Single knock pre-alert ----------

    if(lastKnockID >= 0 && d.knockid !== lastKnockID && !d.strike)
    {
      if(d.kbeep)
        knockPattern();

      flashKnock();
    }

    lastKnockID = d.knockid;

    // ---------- Continuous strike alarm ----------

    if(d.strike)
    {
      if(d.strikeid !== lastPlayedStrikeID || activeAlarmType === "")
      {
        lastPlayedStrikeID = d.strikeid;
        startContinuousAlarm(d.type);
      }
    }
    else
    {
      stopContinuousAlarm();
    }

    // ---------- Replay availability ----------

    lastStrikeID = d.strikeid;

    el("viewStrike").disabled = !d.strikeready;

    if(view === "strike" && d.strikeready && replayID !== d.strikeid)
    {
      await setView("strike");
    }

    // ---------- Live graph ----------

    addNoisePoints(d);

    liveEnergy.push(d.energy);
    liveThr.push(d.knockthr);
    liveTilt.push(d.tilt);
    liveAllow.push(d.allow);

    if(liveEnergy.length > maxPoints)
    {
      liveEnergy.shift();
      liveThr.shift();
      liveTilt.shift();
      liveAllow.shift();
    }

    if(view === "live")
      redraw();
  }
  catch(e)
  {
    el("connState").innerHTML = "&#9679; NO CONNECTION";
    el("connState").className = "red";
  }
}

// Ask again only after the previous answer arrived
async function pollLoop()
{
  await updateData();
  setTimeout(pollLoop, 100);
}

resizeCanvas();
pollLoop();
noiseLoop();

</script>

<!-- SETTINGS DRAWER -->

<div class="drawer" id="drawer">

<div class="drawerHead">
<div class="drawerTitle">&#9881; LIVE SETTINGS</div>
<button class="closeBtn" onclick="closeSettings()">&times;</button>
</div>

<div class="setHelp">Changes work immediately. Watch the live values and the dashed lines on the graph.</div>

<div class="setGroup">LIVE NOW</div>

<div class="liveBox">
<div class="liveItem"><div class="gLabel">KNOCK / LIMIT</div><b id="lvKnock">&ndash;</b></div>
<div class="liveItem"><div class="gLabel">TILT / PULL LINE</div><b id="lvTilt">&ndash;</b></div>
<div class="liveItem"><div class="gLabel">SLOW PULL</div><b id="lvSlow">&ndash;</b></div>
<div class="liveItem"><div class="gLabel">KNOCKS</div><b id="lvKnocks">&ndash;</b></div>
</div>

<div class="setGroup">KNOCKS</div>

<div class="setRow">
<div class="setTop"><span>Knock limit</span><strong id="v_kmul"></strong></div>
<input type="range" id="s_kmul" min="0.5" max="4" step="0.05" oninput="slide('kmul')">
<div class="setEnds"><span>more sensitive</span><span>stricter</span></div>
<div class="setHelp">How far above the normal noise a knock must be.</div>
</div>

<div class="setRow">
<div class="setTop"><span>Repeated bite</span><strong id="v_rep"></strong></div>
<input type="range" id="s_rcnt" min="2" max="5" step="1" oninput="slide('rcnt')">
<input type="range" id="s_rwin" min="2" max="8" step="0.5" oninput="slide('rwin')">
<div class="setHelp">Top: how many knocks. Bottom: within how many seconds.</div>
</div>

<div class="setRow">
<div class="setTop"><span>Sharp strike</span><strong id="v_sharp"></strong></div>
<input type="range" id="s_sharp" min="0.03" max="0.30" step="0.01" oninput="slide('sharp')">
<div class="setEnds"><span>lighter hit</span><span>harder hit</span></div>
<div class="setHelp">A single hit this hard is always a strike.</div>
</div>

<div class="setRow switchRow">
<div><div class="setTop"><span>Single knock beep</span></div><div class="setHelp">Short beep for one knock.</div></div>
<button class="switch" id="t_kbeep" onclick="toggle('kbeep')">ON</button>
</div>

<div class="setGroup">PULLS</div>

<div class="setRow">
<div class="setTop"><span>Pull angle</span><strong id="v_pdeg"></strong></div>
<input type="range" id="s_pdeg" min="0.4" max="5" step="0.1" oninput="slide('pdeg')">
<div class="setEnds"><span>more sensitive</span><span>stricter</span></div>
<div class="setHelp">The tip must lean at least this far. In waves the line goes higher by itself.</div>
</div>

<div class="setRow">
<div class="setTop"><span>Pull time</span><strong id="v_ptime"></strong></div>
<input type="range" id="s_ptime" min="0.5" max="8" step="0.1" oninput="slide('ptime')">
<div class="setEnds"><span>faster alarm</span><span>slower alarm</span></div>
<div class="setHelp">How long the pull must last before the alarm.</div>
</div>

<div class="setRow switchRow">
<div><div class="setTop"><span>Drop-back alarm</span></div><div class="setHelp">Tip springs back, line goes slack.</div></div>
<button class="switch" id="t_drop" onclick="toggle('drop')">ON</button>
</div>

<div class="saveState" id="saveState">&ndash;</div>

<div class="controls">
<button class="action ready" onclick="saveSettings()">SAVE</button>
<button class="action skip" onclick="defaultSettings()">DEFAULT</button>
</div>

</div>

</body>

</html>
)rawliteral";

// =====================================================
// WEB HANDLERS
// =====================================================

void handleRoot()
{
  server.send_P(200, "text/html", webpage);
}

// I'M WATCHING: stop alert, keep protection, learn new rest position
// ---------- Commands (from the dashboard or the watch) ----------

// I'M WATCHING: stop alert, keep protection, learn new rest position
void cmdWatching()
{
  clearStrike();
  startSettling(false);

  Serial.println("I'M WATCHING");
}

void cmdReelIn()
{
  clearStrike();

  rodState = ST_PAUSED;

  armedSince = 0;

  resetSettleWindow();
  resetSettleRing();
  resetDetectors();

  Serial.println();
  Serial.println("REEL IN - DETECTION PAUSED");
}

// Cast done: settle, pull test, then armed
bool cmdReady()
{
  if(rodState != ST_PAUSED)
    return false;

  startSettling(true);

  Serial.println("READY - WAITING FOR STABLE POSITION");

  return true;
}

// Rod adjusted (no new cast): settle, then armed without pull test
void cmdResume()
{
  if(rodState != ST_PAUSED)
    return;

  startSettling(false);

  Serial.println("RESUME - WAITING FOR STABLE POSITION");
}

void cmdRelearn(bool thenPause)
{
  startEnvironmentLearning();

  learnThenPause = thenPause;
}

void cmdSkipPull()
{
  if(rodState == ST_PULLTEST)
  {
    Serial.println("PULL TEST SKIPPED");
    goMonitoring();
  }
}

void handleWatching()
{
  cmdWatching();
  server.send(200, "text/plain", "OK");
}

void handleReelIn()
{
  cmdReelIn();
  server.send(200, "text/plain", "OK");
}

void handleReady()
{
  server.send(200, "text/plain", cmdReady() ? "OK" : "ALREADY ACTIVE");
}

void handleRelearn()
{
  cmdRelearn(false);
  server.send(200, "text/plain", "OK");
}

void handleSkipPull()
{
  cmdSkipPull();
  server.send(200, "text/plain", "OK");
}

void handleSea()
{
  String m = server.arg("m");

  if(m == "auto")
  {
    seaAuto = true;
    updateConditions(true);
  }
  else
  {
    for(int i = 0; i < 3; i++)
    {
      if(m == SEA_NAMES[i])
      {
        seaAuto = false;
        seaState = i;
        updateKnockThreshold();
      }
    }
  }

  server.send(200, "text/plain", "OK");
}

// ---------- Live settings over the web ----------

void sendSettings()
{
  String json = "{";

  for(int i = 0; i < SETTING_COUNT; i++)
  {
    if(i > 0) json += ",";

    json += "\"";
    json += SETTING_INFO[i].key;
    json += "\":";
    json += String(settings.*(SETTING_INFO[i].field), 3);
  }

  json += ",\"saved\":";
  json += settingsSaved() ? "true" : "false";
  json += "}";

  server.send(200, "application/json", json);
}

void handleSettings()
{
  sendSettings();
}

// /set?k=pdeg&v=1.5  - takes effect immediately
void handleSet()
{
  String k = server.arg("k");
  float v = server.arg("v").toFloat();

  for(int i = 0; i < SETTING_COUNT; i++)
  {
    const SettingInfo& info = SETTING_INFO[i];

    if(k == info.key)
      settings.*(info.field) = clampFloat(v, info.low, info.high);
  }

  updateKnockThreshold();

  sendSettings();
}

void handleSaveSettings()
{
  saveSettings();
  sendSettings();
}

void handleDefaultSettings()
{
  settings = DEFAULT_SETTINGS;

  updateKnockThreshold();

  sendSettings();
}

const char* stateName()
{
  switch(rodState)
  {
    case ST_LEARNING: return "learning";
    case ST_PULLTEST: return "pulltest";
    case ST_SETTLING: return "settling";
    case ST_PAUSED: return "paused";
    default: return "monitoring";
  }
}

void addField(String& json, const char* name, float value, int decimals)
{
  json += ",\"";
  json += name;
  json += "\":";
  json += String(value, decimals);
}

void addField(String& json, const char* name, long value)
{
  json += ",\"";
  json += name;
  json += "\":";
  json += String(value);
}

void addField(String& json, const char* name, bool value)
{
  json += ",\"";
  json += name;
  json += "\":";
  json += value ? "true" : "false";
}

void handleData()
{
  unsigned long timeLeft = 0;

  if(rodState == ST_LEARNING)
  {
    unsigned long elapsed = millis() - learningStart;

    if(elapsed < LEARN_TIME)
      timeLeft = (LEARN_TIME - elapsed + 999) / 1000;
  }

  long dirAge = -1;

  if(dirLearnedAt != 0)
    dirAge = (millis() - dirLearnedAt) / 1000;

  String json;
  json.reserve(900);

  json += "{\"state\":\"";
  json += stateName();
  json += "\"";

  addField(json, "strike", strikeActive);

  json += ",\"type\":\"";
  json += strikeType;
  json += "\",\"lasttype\":\"";
  json += lastStrikeType;
  json += "\"";

  addField(json, "strikeid", (long)strikeEventID);
  addField(json, "strikeready", lastStrikeReady);
  addField(json, "knockid", (long)knockAlertID);

  // Highest knock energy since the last poll, so short knocks are never missed
  addField(json, "energy", energyPeakSincePoll, 4);
  energyPeakSincePoll = 0;

  addField(json, "knockthr", knockThreshold, 4);
  addField(json, "tilt", tiltDeg, 2);
  addField(json, "side", sideDeg, 2);
  addField(json, "allow", pullAllow, 2);
  addField(json, "bucket", (long)(pullBucket / bucketSize * 100.0));
  addField(json, "drop", (long)(dropBucket / bucketSize * 100.0));
  addField(json, "knocks", (long)recentKnockCount());
  addField(json, "dir", haveDir);
  addField(json, "dirage", dirAge);
  addField(json, "confidence", (long)settlingConfidence);
  addField(json, "timeleft", (long)timeLeft);
  addField(json, "grace", rodState == ST_SETTLING && millis() < settleGraceUntil);
  addField(json, "ptdeg", totalDeg, 2);
  addField(json, "ptneed", pullTestNeed(), 2);

  addField(json, "sea", (long)seaState);
  addField(json, "seaauto", seaAuto);
  addField(json, "wave", (long)waveLevel);
  addField(json, "waveswing", waveSwing, 2);
  addField(json, "period", wavePeriod, 1);
  addField(json, "wind", (long)windLevel);
  addField(json, "windswing", windSwing, 2);
  addField(json, "windvib", windVib, 4);
  addField(json, "noise", sensorFloor, 4);
  addField(json, "hr", sensorHighRes);

  json += ",\"sname\":\"";
  json += sensorName();
  json += "\"";

  addField(json, "serr", (long)sensorReadErrors);
  addField(json, "tide", (long)tideLevel);
  addField(json, "tiderate", tideRate, 2);

  addField(json, "up", (long)(millis() / 1000));
  addField(json, "armed", armedSince == 0 ? -1L : (long)((millis() - armedSince) / 1000));
  addField(json, "total", (long)totalSeconds());
  addField(json, "kbeep", settings.knockBeep > 0.5);

  addField(json, "peakenergy", strikePeakEnergy, 4);
  addField(json, "peaktilt", strikePeakTilt, 2);

  addVibrationSamples(json);

  addLoraFields(json);

  json += "}";

  server.send(200, "application/json", json);
}

// Last strike replay: 4 s before + 4 s after
void handleStrike()
{
  String json;
  json.reserve(REC_TOTAL * 16 + 200);

  json += "{\"type\":\"";
  json += savedType;
  json += "\"";

  addField(json, "pre", (long)savedPre);
  addField(json, "thr", savedThreshold, 4);
  addField(json, "allow", savedAllow, 2);
  addField(json, "dir", haveDir);

  json += ",\"e\":[";

  for(int i = 0; i < savedCount; i++)
  {
    if(i > 0) json += ",";
    json += String(savedEnergy[i], 4);
  }

  json += "],\"t\":[";

  for(int i = 0; i < savedCount; i++)
  {
    if(i > 0) json += ",";
    json += String(savedTilt[i], 2);
  }

  json += "]}";

  server.send(200, "application/json", json);
}

// =====================================================
// LoRa LINK TO THE WATCH
//
// Heartbeat every second (state, tilt, strike...), STRIKE packets
// repeated until the watch confirms, commands from the watch
// confirmed with an ACK. Never blocks the 100 Hz detection.
// =====================================================

#if ENABLE_LORA

// XIAO ESP32-S3 + Wio-SX1262 kit pins
#define ROD_LORA_SCK      7
#define ROD_LORA_MISO     8
#define ROD_LORA_MOSI     9
#define ROD_LORA_CS      41
#define ROD_LORA_RESET   42
#define ROD_LORA_BUSY    40
#define ROD_LORA_DIO1    39

SX1262 radio = new Module(ROD_LORA_CS, ROD_LORA_DIO1, ROD_LORA_RESET, ROD_LORA_BUSY);

bool loraOk = false;
volatile bool loraIrq = false;
bool loraTxBusy = false;
unsigned long loraTxStart = 0;

uint8_t loraSeq = 0;
unsigned long lastHeartbeatSent = 0;

// Strike waiting for the watch to confirm
bool strikeNeedsAck = false;
uint16_t strikeAckId = 0;
int strikeTries = 0;
unsigned long nextStrikeSend = 0;

// ACK waiting to be sent (short delay so the other side is listening)
bool ackQueued = false;
ss_ack_t queuedAck;
unsigned long ackDueAt = 0;

int lastCommandSeq = -1;

// What the rod knows about the watch (any packet from it counts)
unsigned long watchLastSeen = 0;
float watchRssi = 0;            // how strong the rod hears the watch
float watchSnr = 0;
int watchHearsRodRssi = 0;      // reported by the watch
int watchHearsRodSnr = 0;
bool watchReported = false;

void IRAM_ATTR onLoraIrq()
{
  loraIrq = true;
}

uint8_t strikeCode(const String& type)
{
  if(type == "SHARP STRIKE") return STRIKE_SHARP;
  if(type == "SLOW PULL") return STRIKE_SLOW;
  if(type == "REPEATED BITE") return STRIKE_REPEATED;
  if(type == "DROP-BACK") return STRIKE_DROPBACK;

  return STRIKE_NONE;
}

uint8_t rodStateCode()
{
  switch(rodState)
  {
    case ST_LEARNING: return ROD_LEARNING;
    case ST_PULLTEST: return ROD_PULLTEST;
    case ST_SETTLING: return ROD_SETTLING;
    case ST_PAUSED: return ROD_PAUSED;
    default: return ROD_MONITORING;
  }
}

void loraSend(const void* data, size_t len)
{
  if(radio.startTransmit((const uint8_t*)data, len) == RADIOLIB_ERR_NONE)
  {
    loraTxBusy = true;
    loraTxStart = millis();
  }
  else
  {
    radio.startReceive();
  }
}

void fillHeader(ss_header_t& h, uint8_t type)
{
  h.net = SS_NET_ID;
  h.type = type;
  h.seq = loraSeq++;
}

void sendHeartbeat()
{
  ss_heartbeat_t p;
  fillHeader(p.h, PKT_HEARTBEAT);

  unsigned long learnLeft = 0;

  if(rodState == ST_LEARNING && millis() - learningStart < LEARN_TIME)
    learnLeft = (LEARN_TIME - (millis() - learningStart) + 999) / 1000;

  p.state = rodStateCode();
  p.strike = strikeActive ? strikeCode(strikeType) : (uint8_t)STRIKE_NONE;
  p.strikeId = (uint16_t)strikeEventID;
  p.sea = seaState;
  p.battery = 255;   // not measured yet
  p.learnLeft = (uint8_t)learnLeft;
  p.tilt10 = (int16_t)clampFloat(tiltDeg * 10.0, -32000, 32000);
  p.bucket = (uint8_t)clampFloat(pullBucket / bucketSize * 100.0, 0, 100);
  p.knocks = (uint8_t)recentKnockCount();

  loraSend(&p, sizeof(p));
}

void sendStrike()
{
  ss_strike_t p;
  fillHeader(p.h, PKT_STRIKE);

  p.strike = strikeCode(strikeType);
  p.strikeId = strikeAckId;
  p.peakTilt10 = (int16_t)clampFloat(strikePeakTilt * 10.0, -32000, 32000);

  loraSend(&p, sizeof(p));
}

void queueAck(uint8_t ackType, uint16_t value)
{
  fillHeader(queuedAck.h, PKT_ACK);

  queuedAck.ackType = ackType;
  queuedAck.ackValue = value;

  ackQueued = true;
  ackDueAt = millis() + 20;
}

// Called by startStrike()
void loraOnStrike()
{
  if(!loraOk)
    return;

  strikeNeedsAck = true;
  strikeAckId = (uint16_t)strikeEventID;
  strikeTries = 0;
  nextStrikeSend = millis();
}

void handleCommand(const ss_command_t& c)
{
  // Always confirm, even a repeat (our first ACK may have been lost)
  queueAck(PKT_COMMAND, c.h.seq);

  if(c.h.seq == lastCommandSeq)
    return;

  lastCommandSeq = c.h.seq;

  Serial.printf("[LoRa] command %d from watch\n", c.cmd);

  switch(c.cmd)
  {
    case CMD_LEARN:    cmdRelearn(true); break;
    case CMD_WATCHING: cmdWatching(); break;
    case CMD_REELIN:   cmdReelIn(); break;
    case CMD_READY:    cmdReady(); break;
    case CMD_RESUME:   cmdResume(); break;
    case CMD_SKIPPULL: cmdSkipPull(); break;
  }
}

void handlePacket(const uint8_t* buf, size_t len)
{
  if(len < sizeof(ss_header_t))
    return;

  const ss_header_t* h = (const ss_header_t*)buf;

  if(h->net != SS_NET_ID)
    return;

  watchLastSeen = millis();
  watchRssi = radio.getRSSI();
  watchSnr = radio.getSNR();

  if(h->type == PKT_WATCH_HB && len >= sizeof(ss_watch_hb_t))
  {
    const ss_watch_hb_t* w = (const ss_watch_hb_t*)buf;

    watchHearsRodRssi = w->rodRssi;
    watchHearsRodSnr = w->rodSnr;
    watchReported = true;
  }
  else if(h->type == PKT_COMMAND && len >= sizeof(ss_command_t))
  {
    handleCommand(*(const ss_command_t*)buf);
  }
  else if(h->type == PKT_ACK && len >= sizeof(ss_ack_t))
  {
    const ss_ack_t* a = (const ss_ack_t*)buf;

    if(a->ackType == PKT_STRIKE && a->ackValue == strikeAckId)
    {
      strikeNeedsAck = false;
      Serial.println("[LoRa] watch confirmed the strike");
    }
  }
}

void loraBegin()
{
  SPI.begin(ROD_LORA_SCK, ROD_LORA_MISO, ROD_LORA_MOSI, ROD_LORA_CS);

  // Wio-SX1262: 1.8V TCXO powered from DIO3
  int16_t state = radio.begin(
    SS_FREQ_MHZ, SS_BW_KHZ, SS_SF, SS_CR,
    SS_SYNC_WORD, SS_POWER_DBM, SS_PREAMBLE, 1.8
  );

  if(state != RADIOLIB_ERR_NONE)
  {
    Serial.printf("[LoRa] FAILED, error %d - running without the watch\n", state);
    return;
  }

  // Wio-SX1262 antenna switch is driven by DIO2
  radio.setDio2AsRfSwitch(true);

  radio.setDio1Action(onLoraIrq);
  radio.startReceive();

  loraOk = true;

  Serial.printf("[LoRa] OK  %.3f MHz  SF%d - talking to the watch\n", SS_FREQ_MHZ, SS_SF);
}

void loraPoll()
{
  if(!loraOk)
    return;

  unsigned long now = millis();

  // ---------- Radio finished sending or received something ----------

  if(loraIrq)
  {
    loraIrq = false;

    if(loraTxBusy)
    {
      radio.finishTransmit();
      loraTxBusy = false;
    }
    else
    {
      uint8_t buf[64];
      size_t len = radio.getPacketLength();

      if(len > 0 && len <= sizeof(buf) && radio.readData(buf, len) == RADIOLIB_ERR_NONE)
        handlePacket(buf, len);
    }

    radio.startReceive();
  }

  // Never stay stuck "sending"
  if(loraTxBusy && now - loraTxStart > 2000)
  {
    loraTxBusy = false;
    radio.startReceive();
  }

  if(loraTxBusy)
    return;

  // ---------- What to send next (most important first) ----------

  if(ackQueued && (long)(now - ackDueAt) >= 0)
  {
    ackQueued = false;
    loraSend(&queuedAck, sizeof(queuedAck));
    return;
  }

  if(strikeNeedsAck && strikeActive && (long)(now - nextStrikeSend) >= 0)
  {
    sendStrike();

    strikeTries++;
    nextStrikeSend = now + SS_RESEND_MS + random(0, 300);

    if(strikeTries >= SS_MAX_TRIES)
      strikeNeedsAck = false;

    return;
  }

  if(now - lastHeartbeatSent >= SS_HEARTBEAT_MS)
  {
    lastHeartbeatSent = now;
    sendHeartbeat();
  }
}

// Watch heard in the last 15 s (it reports every 5 s)
bool watchConnected()
{
  return loraOk && watchLastSeen != 0 && millis() - watchLastSeen < 15000;
}

int loraBars(float rssi)
{
  if(rssi > -80)  return 4;
  if(rssi > -95)  return 3;
  if(rssi > -108) return 2;

  return 1;
}

void addLoraFields(String& json)
{
  bool connected = watchConnected();

  addField(json, "lora", loraOk);
  addField(json, "watch", connected);
  addField(json, "wbars", (long)(connected ? loraBars(watchRssi) : 0));
  addField(json, "wrssi", (long)watchRssi);
  addField(json, "wsnr", watchSnr, 1);
  addField(json, "wrep", connected && watchReported);
  addField(json, "w2rssi", (long)watchHearsRodRssi);
  addField(json, "w2snr", (long)watchHearsRodSnr);
  addField(json, "wseen", watchLastSeen == 0 ? -1L : (long)((millis() - watchLastSeen) / 1000));
}

#else

void loraOnStrike() {}
void loraBegin() {}
void loraPoll() {}

void addLoraFields(String& json)
{
  addField(json, "lora", false);
  addField(json, "watch", false);
}

#endif

// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);

  delay(1000);

  Wire.begin();
  Wire.setClock(400000);

  // ADXL355 first, then LIS3DH. Keep trying until one answers.
  while(!startSensor())
  {
    Serial.println("NO SENSOR FOUND (ADXL355 or LIS3DH)");
    Serial.println("  ADXL355: pin 1 -> D5, pin 2 -> D4, pin 5 -> GND, pin 6 -> 3V3,");
    Serial.println("           links on the sensor: pin 3 -> 12, pin 4 -> 11");
    Serial.println("  Trying again in 2 s...");

    delay(2000);
  }

  sensorFloor = SENSOR_FLOOR_DEFAULT;
  knockThreshold = KNOCK_FLOOR;

  loadStoredDirection();
  loadSensorFloor();
  loadSettings();
  loadTotalTime();

  if(haveSensorFloor)
  {
    Serial.print("Sensor noise loaded (g): ");
    Serial.println(sensorFloor, 4);
  }

  if(haveStoredDir)
    Serial.println("Stored pull direction loaded");

  WiFi.softAP(apName, apPassword);

  Serial.println();
  Serial.print("Dashboard: http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/watching", handleWatching);
  server.on("/reelin", handleReelIn);
  server.on("/ready", handleReady);
  server.on("/relearn", handleRelearn);
  server.on("/skippull", handleSkipPull);
  server.on("/sea", handleSea);
  server.on("/strike", handleStrike);
  server.on("/settings", handleSettings);
  server.on("/set", handleSet);
  server.on("/savesettings", handleSaveSettings);
  server.on("/defaultsettings", handleDefaultSettings);

  server.begin();

  loraBegin();

  startEnvironmentLearning();

  lastSampleTime = millis();
}

// =====================================================
// LOOP
//
// The sensor is read every 2.5 ms, averaged, and processed at a
// fixed 100 steps per second. The web server is served in between.
// =====================================================

void loop()
{
  server.handleClient();

  loraPoll();

  oversample();

  maybeSaveTotalTime();

  unsigned long now = millis();

  if(now - lastSampleTime < SAMPLE_MS)
    return;

  // If the web server held us up for long, do not try to catch up
  if(now - lastSampleTime > SAMPLE_MS * 10)
    lastSampleTime = now;
  else
    lastSampleTime += SAMPLE_MS;

  processSample();
}

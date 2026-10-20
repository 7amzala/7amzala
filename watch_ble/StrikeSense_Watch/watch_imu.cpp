// =====================================================
// LOOKING AT THE WATCH (QMI8658 motion sensor on the watch board)
//
// "Looking" = the watch is held at your looking angle (gravity within 30
//   degrees of it). It starts only after a real raise: a wrist turn
//   (gyroscope) or a big angle change in the last second - lying face up
//   on a table is not enough. It ends as soon as you turn away (45 degrees)
//   or when the watch lies perfectly still for 2 s (a table).
// The looking angle: "face up" until you teach yours (TEACH MY LOOK on the
//   RELEARN page): hold the watch as you look at it for 3 s. Kept in flash.
// Prints [IMU] ... on the Serial Monitor.
// =====================================================

#include <Arduino.h>
#include <Preferences.h>
#include "watch_imu.h"
#include "driver/i2c.h"

#define IMU_PORT  I2C_NUM_0           // the touch screen's bus (Touch_Init)

// QMI8658 registers
#define REG_WHO_AM_I  0x00            // 0x05
#define REG_CTRL1     0x02
#define REG_CTRL2     0x03            // accelerometer: range + rate
#define REG_CTRL3     0x04            // gyroscope: range + rate
#define REG_CTRL7     0x08            // sensors on/off
#define REG_AX_L      0x35            // 12 bytes: accel X Y Z, gyro X Y Z (little endian)

static const float LOOK_IN_DEG = 30;      // within this of your looking angle: looking
static const float LOOK_OUT_DEG = 45;     // further than this: not looking
static const float RAISE_DPS = 60;        // a wrist turn at least this fast...
static const float RAISE_DEG = 35;        // ...or the angle changed this much in the last second
static const uint32_t STILL_MS = 2000;    // perfectly still this long = on a table

static uint8_t addr = 0;
static uint32_t lastRead = 0;

static float ref[3] = { 0, 0, 1 };        // looking angle (gravity direction)
static bool refTaught = false;

static bool looking = false;
static int inCount = 0;
static int outCount = 0;
static uint32_t lastTurnMs = 0;           // last fast wrist turn
static float hist[25][3];                 // gravity, last second (25 readings)
static int histN = 0;
static int histI = 0;

static bool still = false;
static uint32_t stillSince = 0;
static float lastG[3] = { 0, 0, 0 };

static int teachState = 0;
static uint32_t teachStart = 0;
static float teachSum[3];
static int teachN = 0;

static uint32_t lastLog = 0;

static bool get(uint8_t reg, uint8_t *buf, size_t n)
{
  return i2c_master_write_read_device(IMU_PORT, addr, &reg, 1, buf, n, pdMS_TO_TICKS(20)) == ESP_OK;
}

static bool put(uint8_t reg, uint8_t v)
{
  uint8_t b[2] = { reg, v };
  return i2c_master_write_to_device(IMU_PORT, addr, b, 2, pdMS_TO_TICKS(20)) == ESP_OK;
}

static float dot(const float *a, const float *b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// angle in degrees between two unit vectors
static float angleDeg(const float *a, const float *b)
{
  float c = dot(a, b);
  c = c > 1 ? 1 : (c < -1 ? -1 : c);
  return acosf(c) * 57.2958f;
}

static void loadRef()
{
  Preferences p;
  p.begin("look", true);
  refTaught = p.getBool("taught", false);

  if(refTaught)
  {
    ref[0] = p.getFloat("x", 0);
    ref[1] = p.getFloat("y", 0);
    ref[2] = p.getFloat("z", 1);
  }

  p.end();
}

static void saveRef()
{
  Preferences p;
  p.begin("look", false);
  p.putBool("taught", true);
  p.putFloat("x", ref[0]);
  p.putFloat("y", ref[1]);
  p.putFloat("z", ref[2]);
  p.end();
}

bool imu_begin(void)
{
  const uint8_t tries[2] = { 0x6B, 0x6A };

  for(int i = 0; i < 2; i++)
  {
    uint8_t id = 0;
    addr = tries[i];

    if(get(REG_WHO_AM_I, &id, 1) && id == 0x05)
    {
      put(REG_CTRL1, 0x40);           // register address auto-increment
      put(REG_CTRL2, 0x17);           // accelerometer +-4 g, 62.5 a second
      put(REG_CTRL3, 0x57);           // gyroscope +-512 deg/s, 62.5 a second
      put(REG_CTRL7, 0x03);           // both on
      loadRef();
      Serial.printf("[IMU] QMI8658 found at 0x%02X - screen on while you look (%s looking angle)\n",
                    addr, refTaught ? "your taught" : "default face-up");
      return true;
    }
  }

  addr = 0;
  Serial.println("[IMU] motion sensor not found - the screen uses a timer instead");
  return false;
}

void imu_poll(void)
{
  if(addr == 0 || millis() - lastRead < 40)
    return;

  uint32_t now = millis();
  lastRead = now;

  uint8_t b[12];

  if(!get(REG_AX_L, b, 12))
    return;

  // +-4 g: 8192 per g;  +-512 deg/s: 64 per deg/s
  float a[3], gy[3];

  for(int i = 0; i < 3; i++)
  {
    a[i] = (int16_t)(b[i * 2] | (b[i * 2 + 1] << 8)) / 8192.0f;
    gy[i] = (int16_t)(b[6 + i * 2] | (b[7 + i * 2] << 8)) / 64.0f;
  }

  float len = sqrtf(dot(a, a));

  if(len < 0.3f)
    return;                           // falling / bad reading

  float g[3] = { a[0] / len, a[1] / len, a[2] / len };
  float turn = sqrtf(dot(gy, gy));    // deg/s

  // ---- still (table)? tiny gyro and no change in tilt ----
  float moved = angleDeg(g, lastG);
  memcpy(lastG, g, sizeof(g));

  if(turn < 1.5f && moved < 0.3f)
  {
    if(stillSince == 0)
      stillSince = now;
  }
  else
  {
    stillSince = 0;
  }

  still = stillSince != 0 && now - stillSince >= STILL_MS;

  // ---- teaching the looking angle ----
  if(teachState == 1)
  {
    if(turn > 25)
    {
      teachState = 3;                 // moved too much: try again
      Serial.println("[IMU] teach: moved too much, try again");
    }
    else
    {
      for(int i = 0; i < 3; i++)
        teachSum[i] += g[i];

      teachN++;

      if(now - teachStart >= 3000)
      {
        float l = sqrtf(dot(teachSum, teachSum));

        for(int i = 0; i < 3; i++)
          ref[i] = teachSum[i] / l;

        refTaught = true;
        saveRef();
        teachState = 2;
        looking = true;
        Serial.printf("[IMU] looking angle learned: %.2f %.2f %.2f\n", ref[0], ref[1], ref[2]);
      }
    }
  }

  // ---- looking? ----
  if(turn > RAISE_DPS)
    lastTurnMs = now;

  // biggest change of angle in the last second
  float swing = 0;

  for(int i = 0; i < histN; i++)
  {
    float d = angleDeg(g, hist[i]);

    if(d > swing)
      swing = d;
  }

  memcpy(hist[histI], g, sizeof(g));
  histI = (histI + 1) % 25;

  if(histN < 25)
    histN++;

  float off = angleDeg(g, ref);

  // default (not taught): face up either way round
  if(!refTaught)
  {
    float down[3] = { -ref[0], -ref[1], -ref[2] };
    float off2 = angleDeg(g, down);

    if(off2 < off)
      off = off2;
  }

  bool raise = now - lastTurnMs < 1000 || swing > RAISE_DEG;

  if(!looking)
  {
    inCount = off < LOOK_IN_DEG ? inCount + 1 : 0;

    if(inCount >= 3 && raise && !still)
    {
      looking = true;
      outCount = 0;
      Serial.printf("[IMU] looking (%.0f deg from your angle)\n", off);
    }
  }
  else if(teachState != 1)
  {
    outCount = off > LOOK_OUT_DEG ? outCount + 1 : 0;

    if(outCount >= 2 || still)
    {
      looking = false;
      inCount = 0;
      Serial.printf("[IMU] not looking (%s)\n", still ? "lying still" : "turned away");
    }
  }

  // once a minute: what the sensor sees (to tune)
  if(now - lastLog > 60000)
  {
    lastLog = now;
    Serial.printf("[IMU] gravity %.2f %.2f %.2f, %.0f deg from your angle, turn %.0f deg/s\n",
                  g[0], g[1], g[2], off, turn);
  }
}

bool imu_present(void)
{
  return addr != 0;
}

bool imu_looking(void)
{
  return looking;
}

bool imu_still(void)
{
  return still;
}

void imu_teach_start(void)
{
  teachState = 1;
  teachStart = millis();
  teachSum[0] = teachSum[1] = teachSum[2] = 0;
  teachN = 0;
  Serial.println("[IMU] teach: hold the watch as you look at it for 3 s");
}

int imu_teach_state(void)
{
  return teachState;
}

float imu_teach_progress(void)
{
  if(teachState != 1)
    return teachState == 2 ? 1 : 0;

  float p = (millis() - teachStart) / 3000.0f;
  return p > 1 ? 1 : p;
}

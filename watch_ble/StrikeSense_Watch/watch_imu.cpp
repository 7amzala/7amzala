// =====================================================
// RAISE TO WAKE (QMI8658 motion sensor on the watch board)
//
// The screen faces up (towards you) when you look at the watch: gravity
// is then mostly on the sensor's Z axis. A "raise" = the watch was NOT
// facing up a moment ago, and now it is, held still for a moment.
// Prints [IMU] ... on the Serial Monitor.
// =====================================================

#include <Arduino.h>
#include "watch_imu.h"
#include "driver/i2c.h"

#define IMU_PORT  I2C_NUM_0           // the touch screen's bus (Touch_Init)

// QMI8658 registers
#define REG_WHO_AM_I  0x00            // 0x05
#define REG_CTRL1     0x02
#define REG_CTRL2     0x03            // accelerometer: range + rate
#define REG_CTRL7     0x08            // sensors on/off
#define REG_AX_L      0x35            // 6 bytes: X, Y, Z (little endian)

static uint8_t addr = 0;
static uint32_t lastRead = 0;
static uint32_t notUpAt = 0;          // last time the watch was not facing up
static int upCount = 0;
static bool wasUp = false;
static volatile bool raised = false;
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
      put(REG_CTRL2, 0x17);           // +-4 g, 62.5 readings a second
      put(REG_CTRL7, 0x01);           // accelerometer on (gyroscope off: saves power)
      Serial.printf("[IMU] QMI8658 found at 0x%02X - raise to wake is on\n", addr);
      return true;
    }
  }

  addr = 0;
  Serial.println("[IMU] motion sensor not found - no raise to wake (touch still wakes the screen)");
  return false;
}

void imu_poll(void)
{
  if(addr == 0 || millis() - lastRead < 40)
    return;

  lastRead = millis();

  uint8_t b[6];

  if(!get(REG_AX_L, b, 6))
    return;

  // +-4 g: 8192 per g
  float x = (int16_t)(b[0] | (b[1] << 8)) / 8192.0f;
  float y = (int16_t)(b[2] | (b[3] << 8)) / 8192.0f;
  float z = (int16_t)(b[4] | (b[5] << 8)) / 8192.0f;

  float az = fabsf(z);
  bool up = az > 0.75f && fabsf(x) < 0.6f && fabsf(y) < 0.6f;

  if(az < 0.5f)
    notUpAt = millis();

  // facing up for 3 readings (~120 ms), and not facing up less than 1.5 s before
  upCount = up ? upCount + 1 : 0;

  if(upCount == 3 && !wasUp && millis() - notUpAt < 1500)
  {
    raised = true;
    Serial.println("[IMU] wrist raised: screen on");
  }

  if(upCount >= 3)
    wasUp = true;
  else if(!up)
    wasUp = false;

  // once a minute: what the sensor sees (to tune the raise)
  if(millis() - lastLog > 60000)
  {
    lastLog = millis();
    Serial.printf("[IMU] x %.2f  y %.2f  z %.2f g\n", x, y, z);
  }
}

bool imu_take_raise(void)
{
  if(!raised)
    return false;

  raised = false;
  return true;
}

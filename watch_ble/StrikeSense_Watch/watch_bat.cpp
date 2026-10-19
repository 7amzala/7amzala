// =====================================================
// WATCH BATTERY (BATTERY page)
//
// Finds how this board measures its battery, at start:
//   - an AXP2101 power chip on the I2C bus (0x34): reads its battery voltage
//   - otherwise the battery voltage on GPIO 4 (divider 1/3, or 1/2)
// Prints what it found on the Serial Monitor as [BAT] ...
// Time left: learned from how fast the % drops (after 15 min and 2 %).
// =====================================================

#include <Arduino.h>
#include "watch_bat.h"
#include "driver/i2c.h"

#define BAT_PORT  I2C_NUM_0           // the touch screen's bus (Touch_Init)
#define AXP_ADDR  0x34
#define ADC_PIN   4

enum { SRC_NONE, SRC_AXP, SRC_ADC };

static int source = SRC_NONE;
static float adcRatio = 3;
static float volts = 0;
static int percent = -1;
static int startPct = -1;
static uint32_t startMs = 0;
static long leftMin = -1;
static uint32_t lastRead = 0;

static bool axpGet(uint8_t reg, uint8_t *v)
{
  return i2c_master_write_read_device(BAT_PORT, AXP_ADDR, &reg, 1, v, 1, pdMS_TO_TICKS(20)) == ESP_OK;
}

static bool axpPut(uint8_t reg, uint8_t v)
{
  uint8_t b[2] = { reg, v };
  return i2c_master_write_to_device(BAT_PORT, AXP_ADDR, b, 2, pdMS_TO_TICKS(20)) == ESP_OK;
}

// LiPo: volts -> % (same curve as the rod)
static int lipoPercent(float v)
{
  static const float V[] = { 3.30, 3.50, 3.61, 3.69, 3.75, 3.79, 3.83, 3.87, 3.92, 3.97, 4.04, 4.12, 4.20 };
  static const int   P[] = {    0,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,   95,  100 };
  const int n = sizeof(V) / sizeof(V[0]);

  if(v <= V[0]) return 0;
  if(v >= V[n - 1]) return 100;

  for(int i = 1; i < n; i++)
  {
    if(v <= V[i])
      return P[i - 1] + (int)((v - V[i - 1]) / (V[i] - V[i - 1]) * (P[i] - P[i - 1]));
  }

  return 100;
}

static float readAxp()
{
  uint8_t hi = 0, lo = 0;

  if(!axpGet(0x34, &hi) || !axpGet(0x35, &lo))
    return 0;

  return (((hi & 0x3F) << 8) | lo) / 1000.0;     // mV
}

static float readAdc(float ratio)
{
  uint32_t sum = 0;

  for(int i = 0; i < 16; i++)
    sum += analogReadMilliVolts(ADC_PIN);

  return sum / 16.0 * ratio / 1000.0;
}

static bool plausible(float v)
{
  return v > 3.0 && v < 4.45;
}

void watch_bat_begin(void)
{
  uint8_t id = 0;

  if(axpGet(0x03, &id))
  {
    axpPut(0x30, 0x0F);                // switch on its voltage measurements
    delay(50);
    float v = readAxp();
    Serial.printf("[BAT] AXP2101 power chip found (id 0x%02X), battery %.2f V\n", id, v);

    if(plausible(v))
    {
      source = SRC_AXP;
      volts = v;
    }
  }

  if(source == SRC_NONE)
  {
    float v3 = readAdc(3);
    float v2 = readAdc(2);
    Serial.printf("[BAT] GPIO %d: %.2f V (x3) / %.2f V (x2)\n", ADC_PIN, v3, v2);

    if(plausible(v3))
    {
      source = SRC_ADC;
      adcRatio = 3;
      volts = v3;
    }
    else if(plausible(v2))
    {
      source = SRC_ADC;
      adcRatio = 2;
      volts = v2;
    }
  }

  if(source == SRC_NONE)
  {
    Serial.println("[BAT] watch battery: not measured (no battery, or this board has no battery reading)");
    return;
  }

  percent = lipoPercent(volts);
  Serial.printf("[BAT] watch battery %.2f V = %d %% (%s)\n", volts, percent,
                source == SRC_AXP ? "AXP2101" : adcRatio == 3 ? "GPIO 4, x3" : "GPIO 4, x2");
}

void watch_bat_poll(void)
{
  if(source == SRC_NONE || millis() - lastRead < 10000)
    return;

  lastRead = millis();

  float v = source == SRC_AXP ? readAxp() : readAdc(adcRatio);

  if(!plausible(v))
    return;

  volts = volts * 0.8 + v * 0.2;
  percent = lipoPercent(volts);

  // time left: from the drop since the start (or since it was charged)
  if(startPct < 0 || percent > startPct + 3)
  {
    startPct = percent;
    startMs = millis();
    leftMin = -1;
  }
  else
  {
    float minutes = (millis() - startMs) / 60000.0;
    int drop = startPct - percent;

    if(minutes >= 15 && drop >= 2)
      leftMin = (long)(percent / (drop / minutes));
  }
}

int watch_bat_percent(void)
{
  return percent;
}

long watch_bat_left_min(void)
{
  return leftMin;
}

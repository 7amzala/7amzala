// =====================================================
// WATCH CLOCK: time and date
//
// The phone app sends the time to the rod when it connects, the rod puts
// it in its live data, and the watch keeps it in the board's clock chip
// (PCF85063 at 0x51 on the touch bus), which keeps counting while the
// watch has its battery - also after a restart. Without the chip the
// watch counts in software (until it restarts).
// The time zone (minutes from UTC, with summer time) is kept in flash.
// =====================================================

#include <Arduino.h>
#include <Preferences.h>
#include "watch_clock.h"
#include "driver/i2c.h"

#define RTC_PORT  I2C_NUM_0                // the touch screen's bus (Touch_Init)
#define RTC_ADDR  0x51
#define REG_SEC   0x04                     // seconds..years: 0x04..0x0A (BCD), bit 7 of seconds = clock stopped

static bool chip = false;
static uint32_t base_utc = 0;              // known time...
static uint32_t base_ms = 0;               // ...at this millis()
static int tz = 0;

static uint8_t bcd(uint8_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }
static uint8_t unbcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }

// days since 1970-01-01 <-> year / month / day (proleptic Gregorian)
static long days_from_civil(int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

static void civil_from_days(long z, int *y, unsigned *m, unsigned *d)
{
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long yy = (long)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : -9);
  *y = (int)(yy + (*m <= 2));
}

static bool rtc_read(uint32_t *utc)
{
  uint8_t reg = REG_SEC, b[7];

  if(i2c_master_write_read_device(RTC_PORT, RTC_ADDR, &reg, 1, b, 7, pdMS_TO_TICKS(20)) != ESP_OK)
    return false;

  if(b[0] & 0x80)
    return false;                          // the clock stopped (battery was out): not valid

  int year = 2000 + unbcd(b[6]);
  unsigned month = unbcd(b[5] & 0x1F), day = unbcd(b[3] & 0x3F);

  if(year < 2024 || month < 1 || month > 12 || day < 1 || day > 31)
    return false;

  long days = days_from_civil(year, month, day);
  *utc = (uint32_t)(days * 86400L + unbcd(b[2] & 0x3F) * 3600L + unbcd(b[1] & 0x7F) * 60L + unbcd(b[0] & 0x7F));
  return true;
}

static bool rtc_write(uint32_t utc)
{
  int y; unsigned m, d;
  long days = (long)(utc / 86400);
  uint32_t sec = utc % 86400;

  civil_from_days(days, &y, &m, &d);

  uint8_t b[8] = {
    REG_SEC,
    bcd(sec % 60), bcd((sec / 60) % 60), bcd(sec / 3600),
    bcd(d), (uint8_t)((days + 4) % 7), bcd(m), bcd(y - 2000)
  };

  return i2c_master_write_to_device(RTC_PORT, RTC_ADDR, b, sizeof(b), pdMS_TO_TICKS(20)) == ESP_OK;
}

void clock_begin(void)
{
  Preferences p;
  p.begin("clock", true);
  tz = p.getInt("tz", 0);
  p.end();

  uint32_t utc = 0;
  uint8_t reg = REG_SEC, b;

  chip = i2c_master_write_read_device(RTC_PORT, RTC_ADDR, &reg, 1, &b, 1, pdMS_TO_TICKS(20)) == ESP_OK;

  if(chip && rtc_read(&utc))
  {
    base_utc = utc;
    base_ms = millis();
  }

  Serial.printf("[CLOCK] chip %s, time %s\n", chip ? "found" : "not found (software clock)", base_utc ? "known" : "not set yet");
}

uint32_t clock_now(void)
{
  if(base_utc == 0)
    return 0;

  return base_utc + (millis() - base_ms) / 1000;
}

int clock_tz(void)
{
  return tz;
}

void clock_set(uint32_t utc, int tz_min)
{
  base_utc = utc;
  base_ms = millis();

  if(chip)
    rtc_write(utc);

  if(tz_min != tz)
  {
    tz = tz_min;

    Preferences p;
    p.begin("clock", false);
    p.putInt("tz", tz);
    p.end();
  }
}

// =====================================================
// WRIST VIBRATION (Pimoroni Haptic Bzzz: DRV2605L + LRA motor)
//
// Wiring:  2-5V -> 3V3,  GND -> GND,  SDA -> 43,  SCL -> 44,  TRIG: free.
// Own I2C bus (the touch screen's bus is not on the header).
// The vibrations are the DRV2605L's built-in effects (LRA library).
// =====================================================

#include "buzz.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2c.h"
#include <stdio.h>

#define BUZZ_PORT  I2C_NUM_1
#define BUZZ_SDA   43
#define BUZZ_SCL   44
#define BUZZ_ADDR  0x5A

// DRV2605L registers
#define REG_STATUS   0x00
#define REG_MODE     0x01
#define REG_RTP      0x02
#define REG_LIBRARY  0x03
#define REG_SEQ      0x04          // 8 slots: effect number, or 0x80 | wait (x 10 ms), 0 = end
#define REG_GO       0x0C
#define REG_FEEDBACK 0x1A

static bool found = false;

static bool put(uint8_t reg, uint8_t value)
{
  uint8_t b[2] = { reg, value };
  return i2c_master_write_to_device(BUZZ_PORT, BUZZ_ADDR, b, 2, 20 / portTICK_PERIOD_MS) == ESP_OK;
}

static bool get(uint8_t reg, uint8_t *value)
{
  return i2c_master_write_read_device(BUZZ_PORT, BUZZ_ADDR, &reg, 1, value, 1, 20 / portTICK_PERIOD_MS) == ESP_OK;
}

bool buzz_begin(void)
{
  i2c_config_t conf = {
    .mode = I2C_MODE_MASTER,
    .sda_io_num = BUZZ_SDA,
    .scl_io_num = BUZZ_SCL,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master = { .clk_speed = 400 * 1000 },
    .clk_flags = 0,
  };

  if(i2c_param_config(BUZZ_PORT, &conf) != ESP_OK || i2c_driver_install(BUZZ_PORT, conf.mode, 0, 0, 0) != ESP_OK)
  {
    printf("[BUZZ] I2C start failed\n");
    return false;
  }

  uint8_t status = 0;

  if(!get(REG_STATUS, &status))
  {
    printf("[BUZZ] not found (no vibration) - wiring: SDA 43, SCL 44, 3V3, GND\n");
    return false;
  }

  uint8_t feedback = 0;
  get(REG_FEEDBACK, &feedback);

  found = put(REG_MODE, 0x00) &&                   // awake, effects started by GO
          put(REG_RTP, 0x00) &&
          put(REG_FEEDBACK, feedback | 0x80) &&    // LRA motor (not ERM)
          put(REG_LIBRARY, 6);                     // LRA effect library

  printf("[BUZZ] %s (chip id %d)\n", found ? "found" : "found, but setup failed", status >> 5);

  if(found)
    buzz_play(BUZZ_HELLO);

  return found;
}

void buzz_play(buzz_t what)
{
  if(!found)
    return;

  // effect numbers from the DRV2605L effect list
  static const uint8_t seq[][8] = {
    [BUZZ_TAP]    = { 24, 0 },                               // sharp tick
    [BUZZ_KNOCK]  = { 1, 0 },                                // strong click
    [BUZZ_PULL]   = { 7, 0 },                                // soft bump
    [BUZZ_DROP]   = { 1, 0x80 | 12, 1, 0 },                  // click, 120 ms, click
    [BUZZ_STRIKE] = { 47, 47, 0x80 | 8, 47, 47, 0 },         // buzz buzz . buzz buzz
    [BUZZ_LOST]   = { 47, 0x80 | 40, 47, 0x80 | 40, 47, 0 }, // 3 slow buzzes
    [BUZZ_HELLO]  = { 10, 0 },                               // double click
  };

  put(REG_GO, 0);                 // stop what is playing

  for(int i = 0; i < 8; i++)
  {
    put(REG_SEQ + i, seq[what][i]);

    if(seq[what][i] == 0)
      break;
  }

  put(REG_GO, 1);
}

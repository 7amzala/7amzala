// =====================================================
// WRIST VIBRATION (Pimoroni Haptic Bzzz: DRV2605L + LRA motor)
//
// Wiring (watch header, right row):  2-5V -> 3V3,  GND -> GND,
//   SDA -> SDA,  SCL -> SCL,  TRIG: not connected.
// Shares the watch's I2C bus with the touch screen (0x38); the buzzer is 0x5A.
// To use two other pins instead: BUZZ_OWN_BUS 1 and set BUZZ_SDA / BUZZ_SCL.
//
// At start: lists what is on the I2C bus, then sets the DRV2605L up the
// same way as Pimoroni's own library (drv2605-python): reset, LRA mode,
// LRA effect library, auto-calibration (loop gain high, brake 2, 1 s).
// If calibration fails it drives the motor at a fixed frequency instead.
// Everything is printed on the Serial Monitor as [BUZZ] ...
// =====================================================

#include <Arduino.h>
#include "buzz.h"
#include "driver/i2c.h"

#define BUZZ_OWN_BUS 0              // 0 = header SDA/SCL (touch bus, already started)

#if BUZZ_OWN_BUS
  #define BUZZ_PORT  I2C_NUM_1
  #define BUZZ_SDA   43
  #define BUZZ_SCL   44
#else
  #define BUZZ_PORT  I2C_NUM_0      // same port as FT3168.cpp (Touch_Init)
#endif

#define BUZZ_ADDR  0x5A

// DRV2605L registers
#define REG_STATUS    0x00
#define REG_MODE      0x01
#define REG_RTP       0x02
#define REG_LIBRARY   0x03
#define REG_SEQ       0x04          // 8 slots: effect number, or 0x80 | wait (x 10 ms), 0 = end
#define REG_GO        0x0C
#define REG_RATED_V   0x16
#define REG_OD_CLAMP  0x17
#define REG_CAL_COMP  0x18
#define REG_CAL_BEMF  0x19
#define REG_FEEDBACK  0x1A
#define REG_CONTROL1  0x1B
#define REG_CONTROL2  0x1C
#define REG_CONTROL3  0x1D
#define REG_CONTROL4  0x1E

static bool found = false;

static bool put(uint8_t reg, uint8_t value)
{
  uint8_t b[2] = { reg, value };
  return i2c_master_write_to_device(BUZZ_PORT, BUZZ_ADDR, b, 2, pdMS_TO_TICKS(50)) == ESP_OK;
}

static bool get(uint8_t reg, uint8_t *value)
{
  return i2c_master_write_read_device(BUZZ_PORT, BUZZ_ADDR, &reg, 1, value, 1, pdMS_TO_TICKS(50)) == ESP_OK;
}

// Print every address that answers on the bus (touch = 0x38, buzzer = 0x5A)
static void scanBus()
{
  Serial.print("[BUZZ] I2C bus:");
  int n = 0;

  for(uint8_t a = 0x08; a < 0x78; a++)
  {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (a << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(BUZZ_PORT, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);

    if(r == ESP_OK)
    {
      Serial.printf(" 0x%02X", a);
      n++;
    }
  }

  Serial.println(n == 0 ? " nothing answers" : "");
}

// Wait until GO goes back to 0 (calibration / effect finished)
static bool waitDone(uint32_t ms)
{
  uint32_t t = millis();
  uint8_t go = 1;

  while(millis() - t < ms)
  {
    if(get(REG_GO, &go) && (go & 1) == 0)
      return true;

    delay(10);
  }

  return false;
}

bool buzz_begin(void)
{
#if BUZZ_OWN_BUS
  i2c_config_t conf = {};
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = BUZZ_SDA;
  conf.scl_io_num = BUZZ_SCL;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = 400 * 1000;

  if(i2c_param_config(BUZZ_PORT, &conf) != ESP_OK || i2c_driver_install(BUZZ_PORT, conf.mode, 0, 0, 0) != ESP_OK)
  {
    Serial.println("[BUZZ] I2C start failed");
    return false;
  }
#endif

  scanBus();

  uint8_t status = 0;

  if(!get(REG_STATUS, &status))
  {
    Serial.println("[BUZZ] NOT FOUND at 0x5A - check the wires: 2-5V->3V3, GND->GND, SDA->SDA, SCL->SCL");
    return false;
  }

  Serial.printf("[BUZZ] found DRV2605 (chip id %d)\n", status >> 5);

  // reset (as Pimoroni: wait until the reset bit clears), then wake up
  put(REG_MODE, 0x80);
  delay(100);

  uint8_t mode = 0x80;

  for(int i = 0; i < 50 && get(REG_MODE, &mode) && (mode & 0x80); i++)
    delay(10);

  put(REG_MODE, 0x00);

  // LRA motor (the Bzzz's ELV1411A). Voltages: the chip's defaults, as Pimoroni.
  // Feedback: LRA, brake factor 2, loop gain high, back-EMF gain 2
  put(REG_FEEDBACK, 0x80 | (2 << 4) | (2 << 2) | 2);
  put(REG_LIBRARY, 6);                            // LRA effect library

  // auto-calibration: the chip measures the motor
  uint8_t c2 = 0xF5;
  get(REG_CONTROL2, &c2);
  put(REG_CONTROL2, (c2 & ~0x03) | 0x01);         // IDISS time 1
  put(REG_CONTROL4, 0x30);                        // calibration 1000 ms, zero crossing 100 us
  put(REG_MODE, 0x07);
  put(REG_GO, 1);

  bool calOk = waitDone(3000);
  get(REG_STATUS, &status);
  calOk = calOk && (status & 0x08) == 0;          // DIAG_RESULT 0 = good

  uint8_t comp = 0, bemf = 0;
  get(REG_CAL_COMP, &comp);
  get(REG_CAL_BEMF, &bemf);

  if(calOk)
  {
    Serial.printf("[BUZZ] calibrated (comp %d, bemf %d)\n", comp, bemf);
  }
  else
  {
    // calibration failed: drive the motor at a fixed frequency (open loop)
    uint8_t c3 = 0xA0;
    get(REG_CONTROL3, &c3);
    put(REG_CONTROL3, c3 | 0x01);                 // LRA_OPEN_LOOP
    Serial.printf("[BUZZ] calibration failed (status 0x%02X) - using fixed drive\n", status);
  }

  found = put(REG_MODE, 0x00) &&                  // internal trigger: effects started by GO
          put(REG_RTP, 0x00) &&
          put(REG_LIBRARY, 6);

  if(!found)
  {
    Serial.println("[BUZZ] setup failed");
    return false;
  }

  // Test 1: motor on at full power for 0.4 s (real-time mode, as Pimoroni's
  // trackball example). Feel this one = motor and wiring are fine.
  Serial.println("[BUZZ] test 1: long buzz (full power, 0.4 s)");
  uint8_t c3 = 0xA0;
  get(REG_CONTROL3, &c3);
  put(REG_CONTROL3, c3 | 0x08);                   // real-time data: unsigned 0..255
  put(REG_MODE, 0x05);                            // real-time playback
  put(REG_RTP, 255);
  delay(400);
  put(REG_RTP, 0);
  put(REG_MODE, 0x00);
  delay(300);

  // Test 2: the built-in effect used for alerts
  Serial.println("[BUZZ] test 2: two short buzzes (built-in effect)");
  buzz_play(BUZZ_HELLO);
  Serial.println("[BUZZ] ready");

  return found;
}

static bool live = false;
static int live_level = 0;

// Live vibration: the motor strength is set directly (real-time mode),
// many times a second, so the wrist feels the rod as the fish pulls it.
void buzz_live(int level)
{
  if(!found)
    return;

  level = level < 0 ? 0 : (level > 255 ? 255 : level);

  if(level == 0)
  {
    if(live)
    {
      put(REG_RTP, 0);
      put(REG_MODE, 0x00);          // back to effects
      live = false;
      live_level = 0;
    }

    return;
  }

  if(!live)
  {
    put(REG_GO, 0);
    put(REG_MODE, 0x05);            // real-time playback (unsigned, set in buzz_begin)
    live = true;
    live_level = -1;
  }

  if(level != live_level)
  {
    put(REG_RTP, (uint8_t)level);
    live_level = level;
  }
}

void buzz_play(buzz_t what)
{
  if(!found)
    return;

  if(live)
  {
    put(REG_RTP, 0);                // an effect stops the live vibration
    put(REG_MODE, 0x00);
    live = false;
    live_level = 0;
  }

  // effect numbers from the DRV2605L effect list
  static const uint8_t seq[][8] = {
    { 24, 0 },                                 // TAP: sharp tick
    { 1, 0 },                                  // KNOCK: strong click
    { 7, 0 },                                  // PULL: soft bump
    { 1, 0x80 | 12, 1, 0 },                    // DROP: click, 120 ms, click
    { 47, 47, 0x80 | 8, 47, 47, 0 },           // STRIKE: buzz buzz . buzz buzz
    { 47, 0x80 | 40, 47, 0x80 | 40, 47, 0 },   // LOST: 3 slow buzzes
    { 47, 0x80 | 15, 47, 0 },                  // HELLO: 2 clear buzzes
    { 1, 0x80 | 8, 1, 0x80 | 8, 1, 0 },        // KNOCKS: click click click
    { 47, 0x80 | 12, 1, 0x80 | 12, 47, 0 },    // SLACK: buzz . click . buzz
    { 15, 0x80 | 25, 15, 0x80 | 25, 15, 0 },   // ALARM: 3 x 750 ms strong alert
    { 16, 0x80 | 15, 16, 0x80 | 15, 16, 0 },   // ALARM2: 3 x 1 s strong alert
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

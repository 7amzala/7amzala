// PC harness: the real watch_ui.c + the real LVGL 8.3, drawing into
// memory. ble_link is faked with a scripted rod. Saves the screen at the
// interesting moments as raw RGB565 files.

#include "lvgl.h"
#include "ble_link.h"
#include "watch_ui.h"
#include "watch_bat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 466
#define H 466

static lv_color_t fb[W * H];
static uint32_t sim_ms = 0;
static char sent[64][32];
static int sent_n = 0;

static unsigned long flushed_px = 0;

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c)
{
  flushed_px += (unsigned long)(a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1);
  if(getenv("SHOWAREA") && flushed_px) fprintf(stderr, "%d,%d-%d,%d\n", a->x1, a->y1, a->x2, a->y2);
  for(int y = a->y1; y <= a->y2; y++)
    for(int x = a->x1; x <= a->x2; x++)
      fb[y * W + x] = *c++;

  lv_disp_flush_ready(d);
}

// ---------- fake rod ----------

typedef struct { const char *name; uint32_t from; } scene_t;

static int scene = 0;      // set by main
static uint32_t scene_start = 0;

void ble_link_begin(void) {}
void ble_link_poll(void) {}

// fake vibration: count what was played
#include "buzz.h"
static int buzzed[8];
bool buzz_begin(void) { return true; }
void buzz_play(buzz_t what) { buzzed[what]++; }

// fake screen brightness and motion sensor
static int brightness = 255;
static int bright_changes = 0;
void lcd_set_brightness(uint8_t l) { if(l != brightness) bright_changes++; brightness = l; }
// fake motion sensor: the test says when you look
static int fake_imu = 1, fake_look = 1, fake_still = 0, fake_teach = 0, teach_started = 0;
bool imu_present(void) { return fake_imu; }
bool imu_looking(void) { return fake_look; }
bool imu_still(void) { return fake_still; }
void imu_teach_start(void) { fake_teach = 1; teach_started++; }
int imu_teach_state(void) { return fake_teach; }
float imu_teach_progress(void) { return fake_teach == 1 ? 0.5f : fake_teach == 2 ? 1.0f : 0.0f; }

static int cpu_mhz = 240;
void power_screen(bool on) { cpu_mhz = on ? 240 : 80; }

#include "watch_ui.c"                 // the real screens (statics visible to the test)

// fake watch battery
void watch_bat_begin(void) {}
void watch_bat_poll(void) {}
int watch_bat_percent(void) { return 41; }
long watch_bat_left_min(void) { return 195; }

// fake last strike from the rod
static int ls_asks = 0;
void ble_link_ask_last_strike(void) { ls_asks++; }
bool ble_link_get_last_strike(ble_last_strike_t *o)
{
  memset(o, 0, sizeof(*o));
  o->loaded = true;
  o->have = true;
  strcpy(o->type, "SLOW PULL");
  o->ago_s = 725;
  o->got_ms = 1;
  o->count = 3;
  o->tiltpeak = 3.6f;
  o->pull = 100;
  o->pre = 100;
  o->n = 150;
  for(int i = 0; i < 150; i++)
  {
    o->k[i] = 5 + (i % 7) + (i == 60 ? 120 : 0);
    o->t[i] = i < 70 ? 0 : (i - 70) * 4;
  }
  return true;
}

void ble_link_command(const char *path)
{
  if(sent_n < 64)
    strncpy(sent[sent_n++], path, 31);
}

static float peak = 0;
static long fake_strike_id = 3;       // a new strike gets a new number (as on the rod)

void ble_link_get(ble_link_status_t *s, bool take_peak)
{
  memset(s, 0, sizeof(*s));
  float t = (sim_ms - scene_start) / 1000.0f;
  float ts = sim_ms / 1000.0f;

  s->phase = scene == 0 ? BLE_LINK_SEARCHING : BLE_LINK_CONNECTED;
  if(scene == 0) { s->scans = 7; s->seen = 12; }
  s->seq = scene == 0 ? 0 : sim_ms / 200;
  s->age_ms = 100;
  s->rssi = -64;
  s->bat = 78;
  s->batleft = 580;
  s->knockthr = 0.004f;
  s->wave = 2;
  s->wind = 0;
  s->dir = true;
  s->ptneed = 1.5f;
  strcpy(s->state, "monitoring");

  float e = 0.0012f + 0.0006f * fabsf(sinf(ts * 9.1f) * sinf(ts * 3.3f));
  s->tilt = 0.6f * sinf(ts * 2 * 3.14159f / 7);

  switch(scene)
  {
    case 2:                                     // knock
      s->knockid = t > 1.0f ? 1 : 0;
      s->knocks = t > 1.0f ? 1 : 0;
      if(t > 1.0f && t < 1.3f) e += 0.012f;
      break;

    case 3:                                     // slow pull building
      s->bucket = (int)(t / 6 * 70); if(s->bucket > 70) s->bucket = 70;
      s->tilt += t / 6 * 2.4f;
      break;

    case 4:                                     // strike
      s->strike = true;
      strcpy(s->type, "SLOW PULL");
      s->strikeid = fake_strike_id;
      s->bucket = 100;
      s->tilt = 3.6f;
      e += 0.004f;
      break;

    case 5:                                     // reeled in
      strcpy(s->state, "paused");
      break;

    case 6:                                     // pull test
      strcpy(s->state, "pulltest");
      s->dir = false;
      s->ptdeg = 0.7f;
      break;

    default:
      break;
  }

  if(e > peak)
    peak = e;

  s->energy_peak = peak;

  if(take_peak)
    peak = 0;
}

// ---------- run ----------

static void run_ms(uint32_t ms)
{
  for(uint32_t i = 0; i < ms; i += 5)
  {
    lv_tick_inc(5);
    sim_ms += 5;
    lv_timer_handler();
  }
}

static void save(const char *name)
{
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(NULL);

  char path[64];
  snprintf(path, sizeof(path), "%s.raw", name);
  FILE *f = fopen(path, "wb");
  fwrite(fb, sizeof(fb), 1, f);
  fclose(f);
}

static int touch_x = 0, touch_y = 0, touch_down = 0;

static void touch_read(lv_indev_drv_t *d, lv_indev_data_t *data)
{
  data->point.x = touch_x;
  data->point.y = touch_y;
  data->state = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void press(int x, int y)
{
  touch_x = x; touch_y = y;
  touch_down = 1; run_ms(120);
  touch_down = 0; run_ms(200);
}

// finger from (x1,y) to (x2,y) in 250 ms
static void swipe(int x1, int x2, int y)
{
  touch_y = y;
  touch_down = 1;

  for(int i = 0; i <= 10; i++)
  {
    touch_x = x1 + (x2 - x1) * i / 10;
    run_ms(25);
  }

  touch_down = 0;
  run_ms(200);
}

// finger from (x,y1) to (x,y2) in 250 ms
static void vswipe(int y1, int y2, int x)
{
  touch_x = x;
  touch_down = 1;

  for(int i = 0; i <= 10; i++)
  {
    touch_y = y1 + (y2 - y1) * i / 10;
    run_ms(25);
  }

  touch_down = 0;
  run_ms(200);
}

static int nav_fail = 0;

static void expect(int h, int v, const char *what)
{
  run_ms(200);
  int ok = page == h && vpage == v;
  if(!ok) nav_fail++;
  printf("  %s %s (page %d/%d)\n", ok ? "ok  " : "FAIL", what, page, vpage);
}

static void hold(int x, int y, int ms)
{
  touch_x = x; touch_y = y;
  touch_down = 1; run_ms(ms);
  touch_down = 0; run_ms(200);
}

int main(void)
{
  lv_init();

  static lv_disp_draw_buf_t buf;
  static lv_color_t b1[W * 60];
  lv_disp_draw_buf_init(&buf, b1, NULL, W * 60);

  static lv_disp_drv_t drv;
  lv_disp_drv_init(&drv);
  drv.hor_res = W;
  drv.ver_res = H;
  drv.flush_cb = flush;
  drv.draw_buf = &buf;
  lv_disp_drv_register(&drv);

  static lv_indev_drv_t in;
  lv_indev_drv_init(&in);
  in.type = LV_INDEV_TYPE_POINTER;
  in.read_cb = touch_read;
  lv_indev_drv_register(&in);

  watch_ui_create();

  run_ms(1000);           save("w0_boot");
  scene = 0; run_ms(3000); save("w1_looking");

  scene = 1; scene_start = sim_ms; run_ms(18000);
  flushed_px = 0; run_ms(3000);
  printf("live screen: %.1f full screens redrawn per second\n", flushed_px / 3.0 / (W * H));
  save("w2_armed");
  scene = 2; scene_start = sim_ms; run_ms(2500);  save("w3_knock");
  scene = 3; scene_start = sim_ms; run_ms(7000);  save("w4_pull");
  scene = 4; scene_start = sim_ms; run_ms(1500);  save("w5_strike");
  scene = 5; scene_start = sim_ms; run_ms(1000);  save("w6_reeled");
  scene = 6; scene_start = sim_ms; run_ms(1000);  save("w7_pulltest");

  // tap each button where it is on the screen
  scene = 1; run_ms(500); press(233, 405);   // REEL IN (live)
  scene = 4; run_ms(500); press(233, 290);   // I'M WATCHING (strike)
  int strike_before = buzzed[BUZZ_STRIKE];
  run_ms(4000);
  printf("%s: strike buzz stops after I'M WATCHING\n", buzzed[BUZZ_STRIKE] == strike_before ? "PASS" : "FAIL");
  press(233, 372);                           // REEL IN (strike)
  scene = 5; run_ms(500); press(233, 330);   // READY
  scene = 6; run_ms(500); press(233, 360);   // SKIP
  press(233, 120);                           // empty spot: nothing

  // swipe map (on the live screen)
  scene = 1; run_ms(500);
  printf("swipes:\n");
  swipe(380, 120, 300);  expect(1, 0, "left: sea state");  save("w8_sea");
  swipe(380, 120, 300);  expect(1, 0, "left again: stays on sea state");
  swipe(120, 380, 300);  expect(0, 0, "right: main");
  swipe(120, 380, 300);  expect(-1, 0, "right: relearn"); save("w9_relearn");
  swipe(120, 380, 300);  expect(-1, 0, "right again: stays on relearn");
  hold(233, 262, 800);   expect(-1, 0, "short press on RELEARN: nothing");
  hold(233, 262, 2300);  expect(0, 0, "hold 2 s: /relearn sent, back to main");
  swipe(120, 380, 300);  expect(-1, 0, "right: relearn");
  swipe(380, 120, 300);  expect(0, 0, "left: main");
  swipe(380, 120, 300);  expect(1, 0, "left: sea state");
  vswipe(150, 400, 233); expect(1, 1, "down: last strike"); save("w10_laststrike");
  vswipe(150, 400, 233); expect(1, 1, "down again: stays");
  swipe(380, 120, 300);  expect(1, 1, "left/right on last strike: nothing");
  vswipe(400, 150, 233); expect(1, 0, "up: back to sea state");
  vswipe(400, 150, 233); expect(1, -1, "up: battery"); save("w11_battery");
  vswipe(400, 150, 233); expect(1, -1, "up again: stays");
  vswipe(150, 400, 233); expect(1, 0, "down: back to sea state");
  swipe(120, 380, 300);  expect(0, 0, "right: main");
  vswipe(150, 400, 233); expect(0, 1, "down from main: last strike");
  run_ms(31000);         expect(0, 0, "30 s later: main by itself");
  printf("%s: swipe map, %d wrong\n", nav_fail ? "FAIL" : "PASS", nav_fail);
  printf("last strike asked %d times\n", ls_asks);
  printf("before last swipe: %d sent\n", sent_n);
  swipe(380, 120, 405);                      // swipe starting on REEL IN: no /reelin
  printf("after last swipe: %d sent\n", sent_n);

  printf("vibration: tap %d, knock %d, pull %d, drop %d, strike %d, lost %d\n",
         buzzed[BUZZ_TAP], buzzed[BUZZ_KNOCK], buzzed[BUZZ_PULL], buzzed[BUZZ_DROP], buzzed[BUZZ_STRIKE], buzzed[BUZZ_LOST]);
  int buzz_ok = buzzed[BUZZ_KNOCK] >= 1 && buzzed[BUZZ_PULL] >= 2 && buzzed[BUZZ_STRIKE] >= 1 && buzzed[BUZZ_TAP] >= 6;
  printf("%s: vibration for knock, slow pull, strike, buttons\n", buzz_ok ? "PASS" : "FAIL");

  // ---- screen on / dark ----
  printf("screen:\n");
  int sfail = 0;
#define SCREEN(cond, what) do { int ok_ = (cond); if(!ok_) sfail++; printf("  %s %s (brightness %d)\n", ok_ ? "ok  " : "FAIL", what, brightness); } while(0)
  scene = 1; scene_start = sim_ms; fake_look = 0; run_ms(10500);
  SCREEN(brightness == 0, "not looking (10 s after the last touch): dark");
  fake_look = 1; run_ms(200);
  SCREEN(brightness == 255, "looking: on");
  run_ms(60000);
  SCREEN(brightness == 255, "still looking after 60 s: still on");
  fake_look = 0; run_ms(200);
  SCREEN(brightness == 0, "looked away: off at once");
  flushed_px = 0; run_ms(5000);
  SCREEN(flushed_px == 0 && cpu_mhz == 80, "dark: nothing drawn, chip at 80 MHz");
  fake_look = 1; run_ms(200);
  SCREEN(brightness == 255 && cpu_mhz == 240 && flushed_px >= W * H, "look again: full speed, whole screen drawn before it shows");
  fake_look = 0; run_ms(200);
  int before = sent_n;
  press(233, 405);
  SCREEN(brightness == 255 && sent_n == before, "touch on the dark screen: on, REEL IN not pressed");
  press(233, 405);
  SCREEN(sent_n == before + 1, "next touch presses REEL IN");
  scene = 5; run_ms(300); scene = 1;           // (REEL IN answered: back to monitoring)
  run_ms(10500);
  SCREEN(brightness == 0, "10 s after the touch: dark");
  scene = 2; scene_start = sim_ms; run_ms(1300);
  SCREEN(brightness == 0, "knock, not looking: stays dark (vibrates)");
  scene = 3; scene_start = sim_ms; run_ms(2000);
  SCREEN(brightness == 255, "slow pull building: on");
  run_ms(40000);
  SCREEN(brightness == 255, "slow pull building: stays on");
  scene = 1; run_ms(300);
  SCREEN(brightness == 0, "slow pull gone, not looking: off");
  fake_strike_id = 4;                           // a new strike
  scene = 4; scene_start = sim_ms; run_ms(40000);
  SCREEN(brightness == 255, "strike: stays on (40 s)");
  press(233, 290);                              // I'M WATCHING
  run_ms(300);
  SCREEN(brightness == 255, "strike answered with a touch: on 10 s more");
  run_ms(10500);
  SCREEN(brightness == 0, "then, not looking: off");
  // teach my look (relearn page)
  scene = 1; fake_look = 1; run_ms(300);
  swipe(120, 380, 300);                         // right: relearn page
  press(233, 368);                              // TEACH MY LOOK
  SCREEN(teach_started == 1, "TEACH MY LOOK starts teaching");
  fake_look = 0; run_ms(1000);
  SCREEN(brightness == 255 && page == -1, "while teaching: on, even if not looking");
  save("w12_teach");
  fake_teach = 2; run_ms(1000);
  save("w13_learned");
  run_ms(2000); fake_look = 1; swipe(380, 120, 300);
  // no motion sensor: the old timer
  fake_imu = 0; fake_look = 0; scene = 1; run_ms(31000);
  SCREEN(brightness == 0, "no motion sensor: dark after 30 s");
  scene = 2; scene_start = sim_ms; run_ms(1300);
  SCREEN(brightness == 255, "no motion sensor: a knock turns it on");
  fake_imu = 1;
  printf("%s: screen on/dark, %d wrong, %d brightness changes\n", sfail ? "FAIL" : "PASS", sfail, bright_changes);
  sent_n -= 2;                                  // (the REEL IN + I'M WATCHING above)

  const char *want[] = { "/reelin", "/watching", "/reelin", "/ready", "/skippull", "/relearn" };
  int ok = sent_n == 6;
  for(int i = 0; i < sent_n; i++) { printf("  tap %d -> %s\n", i + 1, sent[i]); if(i >= 6 || strcmp(sent[i], want[i])) ok = 0; }
  printf("%s: %d commands sent, %s\n", ok ? "PASS" : "FAIL", sent_n, ok ? "all buttons send the right request" : "wrong");
  return 0;
}

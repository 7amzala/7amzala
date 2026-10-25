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
static int buzzed[16];
bool buzz_begin(void) { return true; }
void buzz_play(buzz_t what) { buzzed[what]++; }
static int live_level = 0, live_max = 0, live_frames = 0, live_changes = 0;
static int live_pulses = 0;           // off -> on: one pulse
void buzz_live(int level) { if(level != live_level) live_changes++; if(level > 0 && live_level == 0) live_pulses++; live_level = level; if(level > live_max) live_max = level; if(level > 0) live_frames++; }
void buzz_live_stop(void) { live_level = 0; }

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

// fake clock chip and SD card
#include "watch_clock.h"
#include "strike_sd.h"
static uint32_t fake_utc0 = 0;        // 0 = the watch does not know the time
static int fake_tz = 0, clock_sets = 0, sd_saves = 0;
uint32_t clock_now(void) { return fake_utc0 ? fake_utc0 + sim_ms / 1000 : 0; }
int clock_tz(void) { return fake_tz; }
void clock_set(uint32_t utc, int tz) { fake_utc0 = utc - sim_ms / 1000; fake_tz = tz; clock_sets++; }
void strike_sd_save(void);
bool strike_sd_present(void) { return true; }

#include "strike_log.c"               // the real strike recorder
#include "watch_ui.c"                 // the real screens (statics visible to the test)

void strike_sd_save(void) { slog_t *r = slog_unsaved(); if(r) { r->saved = true; sd_saves++; } }

// fake watch battery
void watch_bat_begin(void) {}
void watch_bat_poll(void) {}
int watch_bat_percent(void) { return 41; }
long watch_bat_left_min(void) { return 195; }

// fake last strike from the rod
static int ls_asks = 0;
static int fake_ls_recording = 0;
static uint32_t fake_ls_got = 1;
void ble_link_ask_last_strike(void) { ls_asks++; }
bool ble_link_get_last_strike(ble_last_strike_t *o)
{
  memset(o, 0, sizeof(*o));
  o->loaded = true;
  o->have = true;
  strcpy(o->type, "SLOW PULL");
  o->ago_s = 725;
  o->count = 3;
  o->tiltpeak = 3.6f;
  o->pull = 100;
  o->pre = 100;
  o->n = fake_ls_recording ? 112 : 150;      // recording: only 12 points after so far
  o->recording = fake_ls_recording;
  for(int i = 0; i < 150; i++)
  {
    o->k[i] = 5 + (i % 7) + (i == 60 ? 120 : 0);
    o->t[i] = i < 70 ? 0 : (i - 70) * 4;
  }
  // zoom: 2 s + 2 s at 50 a second, a sharp hit at the strike
  o->zn = 200;
  o->zpre = 100;
  for(int i = 0; i < 200; i++)
  {
    o->zk[i] = 8 + (i % 5) + (i >= 98 && i <= 103 ? 220 - abs(i - 100) * 30 : 0);
    o->zt[i] = i < 100 ? 20 : 20 + (i - 100) * 2;
  }
  o->got_ms = fake_ls_got;
  return true;
}

void ble_link_command(const char *path)
{
  if(sent_n < 64)
    strncpy(sent[sent_n++], path, 31);
}

static float peak = 0;
static long fake_rod_time = 0;        // the rod's clock (0 = it does not know)
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
  s->lmode = 0;                // the rod's link choice: Auto
  s->chm = 0;                  // LoRa channel: Auto
  s->bat = 78;
  s->batleft = 580;
  s->knockthr = 0.004f;
  s->allow = 1.0f;
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

    case 7:                                     // strike, fish quiet
      s->strike = true;
      strcpy(s->type, "SLOW PULL");
      s->strikeid = fake_strike_id;
      break;

    case 8:                                     // strike, fish knocking
      s->strike = true;
      strcpy(s->type, "REPEATED BITE");
      s->strikeid = fake_strike_id;
      if(((int)(t * 10)) % 12 == 0) e += 0.009f;
      s->knocks = 2;
      break;

    case 9:                                     // strike, line went slack
      s->strike = true;
      strcpy(s->type, "DROP-BACK + PULL");
      s->strikeid = fake_strike_id;
      s->drop = 60;
      s->tilt = -1.6f;
      break;

    case 10:                                    // strike: hard pull, then the tip settles back
      s->strike = true;
      strcpy(s->type, "SHARP STRIKE");
      s->strikeid = fake_strike_id;
      s->bucket = 100;                          // (stays at 100 on the rod during a strike)
      s->tilt = t < 2 ? 3.6f : 0.1f;
      break;

    case 11:                                    // strike: rod bouncing (fish pulling), then still but a bit bent
      s->strike = true;
      strcpy(s->type, "SLOW PULL");
      s->strikeid = fake_strike_id;
      s->bucket = 100;
      s->tilt = t < 5 ? 0.6f + 0.8f * sinf(2 * 3.14159f * 2 * t) : 1.2f;
      break;

    case 12:                                    // strike: pulled, then roughly back (not exactly) and still
      s->strike = true;
      strcpy(s->type, "SHARP STRIKE");
      s->strikeid = fake_strike_id;
      s->bucket = 100;
      s->tilt = t < 2 ? 1.0f + 0.9f * sinf(2 * 3.14159f * 2 * t) : 0.7f - 0.02f * (t - 2);   // creeping back very slowly
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
  s->ftilt = s->tilt;                           // the rod's quicker tilt: same here
  s->time = fake_rod_time ? fake_rod_time + sim_ms / 1000 : 0;
  s->tz = 60;

  if(take_peak)
    peak = 0;
}

// ---------- run ----------

// LVGL memory: the most ever used (the watch has 48 KB)
static int mem_peak = 0;
static int mem_low_biggest = 1 << 30;

static void mem_check(void)
{
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  int used = (int)(m.total_size - m.free_size);
  if(used > mem_peak) mem_peak = used;
  if((int)m.free_biggest_size < mem_low_biggest) mem_low_biggest = (int)m.free_biggest_size;
}

static void run_ms(uint32_t ms)
{
  for(uint32_t i = 0; i < ms; i += 5)
  {
    lv_tick_inc(5);
    sim_ms += 5;
    lv_timer_handler();

    if(sim_ms % 50 == 0)
      mem_check();
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
  {
    lv_mem_monitor_t m; lv_mem_monitor(&m);
    printf("LVGL memory after start: %d of %d bytes used (%d%%), biggest free %d\n",
           (int)(m.total_size - m.free_size), (int)m.total_size, m.used_pct, (int)m.free_biggest_size);
  }

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
  scene = 1; run_ms(500); press(233, 362);   // REEL IN (live)
  scene = 4; run_ms(500); press(176, 354);   // I'M WATCHING (strike)
  int strike_before = buzzed[BUZZ_STRIKE];
  run_ms(4000);
  printf("%s: strike buzz stops after I'M WATCHING\n", buzzed[BUZZ_STRIKE] == strike_before ? "PASS" : "FAIL");

  press(290, 354);                           // REEL IN (strike)
  scene = 5; run_ms(500); press(233, 268);   // READY
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
  hold(233, 124, 800);   expect(-1, 0, "short press on RELEARN: nothing");
  hold(233, 124, 2300);  expect(0, 0, "hold 2 s: /relearn sent, back to main");
  swipe(120, 380, 300);  expect(-1, 0, "right: setup page");
  hold(233, 188, 2300);  expect(0, 0, "hold NEW PULL DIRECTION 2 s: /teachdir sent, back to main");
  swipe(120, 380, 300);  expect(-1, 0, "right: relearn");
  swipe(380, 120, 300);  expect(0, 0, "left: main");
  swipe(380, 120, 300);  expect(1, 0, "left: sea state");
  vswipe(150, 400, 233); expect(1, 1, "down: last strike"); save("w10_laststrike");
  {
    // the watch's own recording of the strike (no loading from the rod)
    slog_t *r = slog_get(0);
    int own = slog_count() == 1 && r && r->done && r->n > HIST_CLOSE && !lv_obj_has_flag(ls_chart, LV_OBJ_FLAG_HIDDEN);
    int close_first = lv_chart_get_point_count(ls_chart) == HIST_CLOSE;
    int off0 = hist_off;
    swipe(330, 130, 190);                     // drag the graph to the left: later in the strike
    int dragged = hist_off > off0 && lv_chart_get_point_count(ls_chart) == HIST_CLOSE;
    press(233, 190);                          // tap: the whole strike
    int whole = lv_chart_get_point_count(ls_chart) == (r->n < HIST_W ? r->n : HIST_W);
    save("w10b_overview");
    press(233, 190);
    int back = lv_chart_get_point_count(ls_chart) == HIST_CLOSE;
    printf("  %s the watch's own recording: %d points (%.1f s), shown at once\n", own ? "ok  " : "FAIL", r ? r->n : 0, r ? r->n / 10.0 : 0);
    printf("  %s close-up first (6 s), drag moves in time (%d -> %d), tap: whole, tap: close-up\n",
           close_first && dragged && whole && back ? "ok  " : "FAIL", off0, hist_off);
    save("w10c_recording");
    if(!(own && close_first && dragged && whole && back)) nav_fail++;
  }
  vswipe(150, 400, 233); expect(1, 1, "down again: stays");
  swipe(380, 120, 300);  expect(1, 1, "left/right on last strike: nothing");
  vswipe(400, 150, 233); expect(1, 0, "up: back to sea state");
  vswipe(400, 150, 233); expect(1, -1, "up: battery"); save("w11_battery");
  vswipe(400, 150, 233); expect(1, -1, "up again: stays");
  vswipe(150, 400, 233); expect(1, 0, "down: back to sea state");
  swipe(120, 380, 300);  expect(0, 0, "right: main");
  // bad touch reads (shared bus): one far point while the finger rests, or alone
  touch_x = 233; touch_y = 300; touch_down = 1; run_ms(100);
  touch_x = 460; run_ms(30); touch_x = 233; run_ms(100);
  touch_down = 0; run_ms(200);
  expect(0, 0, "finger resting + one bad far point: no page change");
  touch_x = 20; touch_y = 240; touch_down = 1; run_ms(25); touch_x = 440; run_ms(25); touch_down = 0; run_ms(200);
  expect(0, 0, "a bad 50 ms 'touch' jumping across: no page change");
  vswipe(150, 400, 233); expect(0, 1, "down from main: last strike");
  run_ms(31000);         expect(0, 0, "30 s later: main by itself");
  printf("%s: swipe map, %d wrong\n", nav_fail ? "FAIL" : "PASS", nav_fail);
  printf("last strike asked %d times\n", ls_asks);
  printf("before last swipe: %d sent\n", sent_n);
  swipe(380, 120, 362);                      // swipe starting on REEL IN: no /reelin
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
  press(233, 362);
  SCREEN(brightness == 255 && sent_n == before, "touch on the dark screen: on, REEL IN not pressed");
  press(233, 362);
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
  press(176, 354);                              // I'M WATCHING
  run_ms(300);
  SCREEN(brightness == 255, "strike answered with a touch: on 10 s more");
  run_ms(10500);
  SCREEN(brightness == 0, "then, not looking: off");
  // teach my look (relearn page)
  scene = 1; fake_look = 1; run_ms(300);
  swipe(120, 380, 300);                         // right: relearn page
  press(233, 252);                              // TEACH MY LOOK
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

  // ---- STRIKE screen: what the fish does ----
  printf("strike screen:\n");
  int ffail = 0;
#define FISH(cond, what) do { int ok_ = (cond); if(!ok_) ffail++; printf("  %s %s (state %d, strength %.2f, trend %d)\n", ok_ ? "ok  " : "FAIL", what, fish, fish_fast, fish_trend); } while(0)
  fake_look = 1;
  fake_strike_id = 10; scene = 4; scene_start = sim_ms;
  int b0 = buzzed[BUZZ_STRIKE];
  run_ms(4000);
  FISH(fish == FISH_RUN && fish_fast > 0.5f, "pulling hard: RUN, strong");
  { int p0 = live_pulses; live_max = 0; run_ms(1000);
    FISH(buzzed[BUZZ_STRIKE] - b0 >= 1 && live_pulses - p0 >= 12 && live_max >= 240, "RUN, hard pull: bz-bz-bz pulses, fast (12+ a second) and strong"); }
  save("w14_strike_run");
  scene = 9; scene_start = sim_ms; int d0 = buzzed[BUZZ_SLACK]; run_ms(4000);
  FISH(fish == FISH_SLACK && buzzed[BUZZ_SLACK] > d0, "line went loose: SLACK, buzz-click-buzz");
  save("w15_strike_slack");
  scene = 8; scene_start = sim_ms; int k0 = buzzed[BUZZ_KNOCK] + buzzed[BUZZ_KNOCKS]; run_ms(4000);
  FISH(fish == FISH_KNOCK && buzzed[BUZZ_KNOCK] + buzzed[BUZZ_KNOCKS] > k0, "bites: KNOCK, click click click");
  save("w16_strike_knock");
  scene = 7; scene_start = sim_ms; int t0 = buzzed[BUZZ_ALARM]; run_ms(6000);
  FISH(fish == FISH_QUIET && buzzed[BUZZ_ALARM] > t0 && fish_trend <= 0 && live_level == 0, "fish stopped: QUIET, wake-up alarm, live vibration off");
  save("w17_strike_quiet");
  int r0 = buzzed[BUZZ_ALARM]; run_ms(21000);
  FISH(buzzed[BUZZ_ALARM] >= r0 + 3, "still quiet, not answered: alarm every 5 s");
  int r2 = buzzed[BUZZ_ALARM2]; run_ms(10000);
  FISH(buzzed[BUZZ_ALARM2] >= r2 + 2, "after 30 s quiet: longer alarm every 4 s");
  int st_ok = !lv_obj_has_flag(st_panel, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(title, LV_OBJ_FLAG_HIDDEN);
  FISH(st_ok, "strike screen shows the graph panel (no flashing red page)");
  press(176, 354);                              // I'M WATCHING
  int all0 = 0; for(int i = 0; i < 16; i++) if(i != BUZZ_TAP) all0 += buzzed[i];
  run_ms(31000);
  int all1 = 0; for(int i = 0; i < 16; i++) if(i != BUZZ_TAP) all1 += buzzed[i];
  FISH(all1 == all0, "answered: no more buzzing");
  sent_n--;                                     // (that I'M WATCHING)
  scene = 1; run_ms(500);
  printf("%s: strike screen, %d wrong\n", ffail ? "FAIL" : "PASS", ffail);

  // ---- the ring mirrors the rod tip ----
  printf("ring:\n");
  int sent_before_ring = sent_n;
  int rfail = 0;
#define RING(cond, what) do { int ok_ = (cond); if(!ok_) rfail++; printf("  %s %s (ring %d, fish %d)\n", ok_ ? "ok  " : "FAIL", what, lv_arc_get_value(ring), fish); } while(0)
  scene = 1; scene_start = sim_ms; run_ms(3000);
  RING(abs(lv_arc_get_value(ring)) <= 22, "armed, waves only: ring small");
  scene = 3; scene_start = sim_ms; run_ms(6000);
  RING(lv_arc_get_value(ring) >= 50, "slow pull (tip ~2.5 of 3 deg): ring filled to the right");
  scene = 1; run_ms(400);
  RING(lv_arc_get_value(ring) <= 22, "pull let go: ring back within 0.4 s");
  fake_strike_id = 20; scene = 10; scene_start = sim_ms; run_ms(1500);
  RING(lv_arc_get_value(ring) == 100 && fish == FISH_RUN, "strike, pulled hard: ring full, RUN");
  run_ms(2000);
  run_ms(2500);
  RING(abs(lv_arc_get_value(ring)) <= 5 && fish == FISH_QUIET && live_level == 0, "tip settled back + calm 2 s: ring empty, QUIET, live vibration off");
  press(176, 354);                    // I'M WATCHING
  fake_strike_id = 21; scene = 9; scene_start = sim_ms; run_ms(1500);
  RING(lv_arc_get_value(ring) == 0 && fish == FISH_SLACK, "line went slack: ring empty, SLACK");
  press(176, 354);
  fake_strike_id = 22; scene = 11; scene_start = sim_ms; run_ms(3000);
  int ch0 = live_changes; run_ms(1500);
  int pp0 = live_pulses; live_max = 0; run_ms(1000);
  RING(fish == FISH_RUN && live_pulses - pp0 >= 6 && live_pulses - pp0 <= 13 && live_max < 255, "rod bouncing (gentler): RUN, pulses slower than a hard pull");
  run_ms(3500);
  RING(fish == FISH_QUIET && live_level == 0, "rod still but a bit bent: QUIET (calm = not running)");
  press(176, 354);
  fake_strike_id = 23; scene = 4; scene_start = sim_ms; run_ms(6000);
  RING(fish == FISH_RUN && lv_arc_get_value(ring) == 100, "rod still but strongly bent (steady run): still RUN");
  press(176, 354);
  scene = 1; run_ms(500);
  press(176, 354);
  fake_strike_id = 24; scene = 12; scene_start = sim_ms; run_ms(1900);
  int before_still = lv_arc_get_value(ring);
  run_ms(1300);
  RING(before_still > 0 && lv_arc_get_value(ring) <= 3, "rod roughly back (not exactly) and still: ring empty within ~1.3 s");
  run_ms(2000);
  RING(fish == FISH_QUIET && live_level == 0, "then QUIET, pulses stop (wake-up alarm instead)");
  press(176, 354);
  scene = 1; run_ms(500);
  sent_n = sent_before_ring;                    // (the WATCHING taps above)
  printf("%s: ring, %d wrong\n", rfail ? "FAIL" : "PASS", rfail);

  // ---- time, strike history, back home ----
  printf("history:\n");
  int hfail = 0, sent_before_hist = sent_n;
#define HIST(cond, what) do { int ok_ = (cond); if(!ok_) hfail++; printf("  %s %s (page %d/%d, strikes %d)\n", ok_ ? "ok  " : "FAIL", what, page, vpage, slog_count()); } while(0)
  fake_look = 1; scene = 1; run_ms(500);
  fake_rod_time = 1760000000; run_ms(300);
  HIST(labs((long)clock_now() - (fake_rod_time + (long)(sim_ms / 1000))) <= 2 && clock_tz() == 60, "the rod's time (from the phone) sets the watch clock");
  fake_rod_time = 0; int s0 = sent_n; run_ms(300);
  HIST(sent_n > s0 && strncmp(sent[sent_n - 1], "/settime?t=17600", 16) == 0 && strstr(sent[sent_n - 1], "&tz=60"), "rod restarted: the watch tells it the time");
  int n0 = slog_count(), saves0 = sd_saves;
  for(int i = 0; i < 2; i++)
  {
    fake_strike_id = 30 + i; scene = 4; scene_start = sim_ms; run_ms(3000);
    press(176, 354);                            // I'M WATCHING
    scene = 1; run_ms(25000);                   // quiet: the recording ends
  }
  HIST(slog_count() == n0 + 2 && slog_get(0)->strikeid == 31 && slog_get(0)->done, "2 more strikes recorded by the watch");
  HIST(sd_saves >= saves0 + 2, "saved to the SD card after they ended");
  HIST(strncmp(lv_label_get_text(hint), "last strike ", 12) == 0 && strstr(lv_label_get_text(hint), "ago"), "home screen: 'last strike ... ago'");
  vswipe(150, 400, 233);
  HIST(vpage == 1 && strstr(lv_label_get_text(title), "ago") && strstr(lv_label_get_text(subtitle), ":"), "down: newest strike, with its time and '... ago'");
  save("w18_history");
  vswipe(150, 400, 233);
  HIST(vpage == 2, "down again: the strike before");
  vswipe(400, 150, 233);
  HIST(vpage == 1, "up: newer");
  vswipe(150, 400, 233); vswipe(150, 400, 233);
  press(233, 388);                              // the home button
  HIST(page == 0 && vpage == 0, "home button: straight back to the live graph");
  swipe(380, 120, 300);                         // to the sea state page
  scene = 2; scene_start = sim_ms; run_ms(1500);
  HIST(page == 0 && vpage == 0, "a knock on another page: back to the live graph");
  swipe(380, 120, 300); run_ms(300);
  scene = 2; scene_start = sim_ms; run_ms(1500);
  HIST(page == 1, "another knock within 30 s: stays (no jumping all the time)");
  scene = 1; run_ms(500);
  touch_x = 233; touch_y = 300; touch_down = 1; run_ms(100);    // finger resting on the screen
  scene = 3; scene_start = sim_ms; run_ms(2500);
  HIST(page == 1, "slow pull while a finger is on the screen: waits");
  touch_down = 0; run_ms(300);
  HIST(page == 0 && vpage == 0, "finger lifted: back to the live graph");
  scene = 1; run_ms(2000);
  {
    // no PSRAM switched on: little memory. New strikes must still be kept
    // (the oldest drop out of memory), never "stuck at 2"
    extern int slog_test_limit;
    slog_test_limit = 30000;
    int before = slog_count();
    for(int i = 0; i < 6; i++)
    {
      fake_strike_id = 40 + i; scene = 4; scene_start = sim_ms; run_ms(2500);
      press(176, 354);
      scene = 1; run_ms(14000);
    }
    HIST(slog_get(0)->strikeid == 45 && slog_get(0)->done && slog_get(1)->strikeid == 44, "little memory: every new strike is kept (oldest dropped from memory)");
    printf("        %s (had %d)\n", slog_memory_note(), before);
    slog_test_limit = 0;
  }
  sent_n = sent_before_hist;                    // (WATCHING taps, /settime)
  printf("%s: history, %d wrong\n", hfail ? "FAIL" : "PASS", hfail);

  // SETUP: WATCH LINK and LoRa CHANNEL, synced with the app through the rod
  {
    int before = sent_n;
    scene = 1; run_ms(1500);
    swipe(120, 380, 300); run_ms(400);
    save("w19_setup");
    lv_area_t a;
    lv_obj_get_coords(cell[3], &a);
    int shown = !lv_obj_has_flag(cell[3], LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(cell[4], LV_OBJ_FLAG_HIDDEN);
    press((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2); run_ms(300);
    lv_obj_get_coords(cell[4], &a);
    press((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2); run_ms(300);
    save("w20_setup_changed");
    int good = shown && sent_n == before + 2 && strcmp(sent[before], "/linkmode?m=bt") == 0 && strcmp(sent[before + 1], "/lorachan?m=fast") == 0 &&
               strcmp(lv_label_get_text(cell_sub[3]), "Bluetooth only") == 0 && strcmp(lv_label_get_text(cell_sub[4]), "500 m fast") == 0;
    printf("%s: SETUP watch link + LoRa channel: tap -> %s, %s (shows: %s / %s)\n", good ? "PASS" : "FAIL",
           sent_n > before ? sent[before] : "-", sent_n > before + 1 ? sent[before + 1] : "-",
           lv_label_get_text(cell_sub[3]), lv_label_get_text(cell_sub[4]));
    sent_n = before;
  }

  // a new strike: a touch anywhere on the screen stops the vibration the
  // moment the finger lands (not when it lifts), and tells the rod
  fake_strike_id = 5; scene = 4; scene_start = sim_ms; run_ms(1500);
  {
    int tap_before = buzzed[BUZZ_TAP], sent_before = sent_n;
    touch_x = 233; touch_y = 200; touch_down = 1; run_ms(30);
    int quiet_at_once = buzzed[BUZZ_TAP] > tap_before;
    touch_down = 0; run_ms(200);
    int strike_n = buzzed[BUZZ_STRIKE] + buzzed[BUZZ_ALARM] + buzzed[BUZZ_ALARM2] + buzzed[BUZZ_KNOCKS] + buzzed[BUZZ_SLACK];
    run_ms(6000);
    int strike_n2 = buzzed[BUZZ_STRIKE] + buzzed[BUZZ_ALARM] + buzzed[BUZZ_ALARM2] + buzzed[BUZZ_KNOCKS] + buzzed[BUZZ_SLACK];
    int told = 0;
    for(int i = sent_before; i < sent_n; i++)
      if(strcmp(sent[i], "/watching") == 0) told++;
    told = told == 1;
    printf("%s: strike: a touch anywhere stops the vibration at once (%s), tells the rod (%s), no more buzz (%d)\n",
           quiet_at_once && told && strike_n2 == strike_n ? "PASS" : "FAIL", quiet_at_once ? "yes" : "no", told ? "yes" : "no", strike_n2 - strike_n);
  }

  const char *want[] = { "/reelin", "/watching", "/reelin", "/ready", "/skippull", "/relearn", "/teachdir", "/watching" };
  int ok = 1, n = 0;
  for(int i = 0; i < sent_n; i++)
  {
    if(strncmp(sent[i], "/settime", 8) == 0) continue;   // (the watch passing on the time)
    printf("  tap %d -> %s\n", n + 1, sent[i]);
    if(n >= 8 || strcmp(sent[i], want[n])) ok = 0;
    n++;
  }
  if(n != 8) ok = 0;
  {
    lv_mem_monitor_t m; lv_mem_monitor(&m);
    printf("LVGL memory at the end: %d%% used; most ever used %d bytes, smallest biggest free block %d\n",
           m.used_pct, mem_peak, mem_low_biggest);
    // 64-bit PC numbers are bigger than the watch's 32-bit ones: 49.4 KB
    // here ran on the watch's 48 KB; the pool is 64 KB now (lv_conf.h), so
    // stay under 60 KB here to keep a margin.
    printf("%s: LVGL memory fits the watch's 64 KB (%d bytes here, limit 60000)\n",
           mem_peak <= 60000 ? "PASS" : "FAIL", mem_peak);
  }
  printf("%s: %d commands sent, %s\n", ok ? "PASS" : "FAIL", n, ok ? "all buttons send the right request" : "wrong");
  return 0;
}

// PC harness: the real watch_ui.c + the real LVGL 8.3, drawing into
// memory. ble_link is faked with a scripted rod. Saves the screen at the
// interesting moments as raw RGB565 files.

#include "lvgl.h"
#include "ble_link.h"
#include "watch_ui.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define W 466
#define H 466

static lv_color_t fb[W * H];
static uint32_t sim_ms = 0;
static char sent[64][32];
static int sent_n = 0;

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c)
{
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

void ble_link_command(const char *path)
{
  if(sent_n < 64)
    strncpy(sent[sent_n++], path, 31);
}

static float peak = 0;

void ble_link_get(ble_link_status_t *s, bool take_peak)
{
  memset(s, 0, sizeof(*s));
  float t = (sim_ms - scene_start) / 1000.0f;
  float ts = sim_ms / 1000.0f;

  s->phase = scene == 0 ? BLE_LINK_SEARCHING : BLE_LINK_CONNECTED;
  s->seq = scene == 0 ? 0 : sim_ms / 200;
  s->age_ms = 100;
  s->rssi = -64;
  s->bat = 78;
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
      s->strikeid = 3;
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

  scene = 1; scene_start = sim_ms; run_ms(21000); save("w2_armed");
  scene = 2; scene_start = sim_ms; run_ms(2500);  save("w3_knock");
  scene = 3; scene_start = sim_ms; run_ms(7000);  save("w4_pull");
  scene = 4; scene_start = sim_ms; run_ms(1500);  save("w5_strike");
  scene = 5; scene_start = sim_ms; run_ms(1000);  save("w6_reeled");
  scene = 6; scene_start = sim_ms; run_ms(1000);  save("w7_pulltest");

  // tap each button where it is on the screen
  scene = 1; run_ms(500); press(233, 405);   // REEL IN (live)
  scene = 4; run_ms(500); press(233, 290);   // I'M WATCHING (strike)
  press(233, 372);                           // REEL IN (strike)
  scene = 5; run_ms(500); press(233, 330);   // READY
  scene = 6; run_ms(500); press(233, 360);   // SKIP
  press(233, 120);                           // empty spot: nothing

  const char *want[] = { "/reelin", "/watching", "/reelin", "/ready", "/skippull" };
  int ok = sent_n == 5;
  for(int i = 0; i < 5 && i < sent_n; i++) { printf("  tap %d -> %s\n", i + 1, sent[i]); if(strcmp(sent[i], want[i])) ok = 0; }
  printf("%s: %d commands sent, %s\n", ok ? "PASS" : "FAIL", sent_n, ok ? "all buttons send the right request" : "wrong");
  return 0;
}

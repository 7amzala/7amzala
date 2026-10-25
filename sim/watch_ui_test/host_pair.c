// PC test: the watch's PAIR YOUR ROD screens (pair_ui.c) with the real
// LVGL, a fake Bluetooth list of rods, taps on the keypad.

#include "lvgl.h"
#include "ble_link.h"
#include <stdio.h>
#include <string.h>

#define W 466
#define H 466

static lv_color_t fb[W * H];
static int touch_x, touch_y, touch_down;
static int fake_n = 0, restarted = 0;
static uint32_t started_pin = 0;
static char started_id[12];
static pair_state_t fake_state = PAIR_IDLE;
static int checks = 0, failed = 0;

int ble_link_pair_list(pair_rod_t *out, int max)
{
  static const char *ids[3] = { "ROD-4F2A", "ROD-91C7", "ROD-0B3E" };
  static const int rssi[3] = { -58, -74, -88 };
  int n = fake_n < max ? fake_n : max;
  for(int i = 0; i < n; i++) { strcpy(out[i].id, ids[i]); out[i].rssi = rssi[i]; }
  return n;
}
void ble_link_pair_start(const char *id, uint32_t pin) { strcpy(started_id, id); started_pin = pin; fake_state = PAIR_WORKING; }
pair_state_t ble_link_pair_state(void) { return fake_state; }
void pair_restart(void) { restarted++; }

#include "pair_ui.c"

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c)
{
  for(int y = a->y1; y <= a->y2; y++)
    for(int x = a->x1; x <= a->x2; x++)
      fb[y * W + x] = *c++;
  lv_disp_flush_ready(d);
}

static void touch_read(lv_indev_drv_t *d, lv_indev_data_t *data)
{
  (void)d;
  data->point.x = touch_x;
  data->point.y = touch_y;
  data->state = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static int mem_peak = 0;

static void run_ms(int ms)
{
  for(int i = 0; i < ms; i += 5)
  {
    lv_tick_inc(5);
    lv_timer_handler();
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    if((int)(m.total_size - m.free_size) > mem_peak) mem_peak = (int)(m.total_size - m.free_size);
  }
}

static void tap_obj(lv_obj_t *o)
{
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  touch_x = (a.x1 + a.x2) / 2;
  touch_y = (a.y1 + a.y2) / 2;
  touch_down = 1; run_ms(80);
  touch_down = 0; run_ms(120);
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

static void check(int ok, const char *what)
{
  checks++;
  if(!ok) failed++;
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static void type_pin(const char *pin)
{
  for(const char *p = pin; *p; p++)
    tap_obj(keys[*p == '0' ? 10 : *p - '1']);
}

int main(void)
{
  lv_init();
  static lv_disp_draw_buf_t buf;
  static lv_color_t b1[W * 60];
  lv_disp_draw_buf_init(&buf, b1, NULL, W * 60);
  static lv_disp_drv_t drv;
  lv_disp_drv_init(&drv);
  drv.hor_res = W; drv.ver_res = H; drv.flush_cb = flush; drv.draw_buf = &buf;
  lv_disp_drv_register(&drv);
  static lv_indev_drv_t in;
  lv_indev_drv_init(&in);
  in.type = LV_INDEV_TYPE_POINTER; in.read_cb = touch_read;
  lv_indev_drv_register(&in);

  pair_ui_create();
  run_ms(500);
  save("p0_no_rod");
  check(strstr(lv_label_get_text(info), "Switch your rod on") != NULL, "no rod open for pairing: 'Switch your rod on'");

  fake_n = 3; run_ms(500);
  save("p1_list");
  check(!lv_obj_has_flag(rod_btn[2], LV_OBJ_FLAG_HIDDEN) && strcmp(lv_label_get_text(rod_name[0]), "ROD-4F2A") == 0, "3 rods listed by ID");

  tap_obj(rod_btn[0]);
  check(page == P_PIN && strcmp(lv_label_get_text(head), "ROD-4F2A") == 0, "tap ROD-4F2A: PIN keypad");
  type_pin("4829");
  save("p2_pin");
  check(strcmp(lv_label_get_text(pin_label), "482 9__") == 0, "PIN shows as typed: 482 9__");
  tap_obj(keys[11]);
  check(page == P_PIN, "OK before 6 digits: nothing happens");
  tap_obj(keys[9]);
  type_pin("913");
  check(strcmp(lv_label_get_text(pin_label), "482 913") == 0, "back key removes a digit");
  tap_obj(keys[11]);
  check(page == P_WORKING && started_pin == 482913 && strcmp(started_id, "ROD-4F2A") == 0, "OK: pairing ROD-4F2A with PIN 482913");

  fake_state = PAIR_FAILED; run_ms(500);
  save("p3_failed");
  check(page == P_FAILED, "wrong PIN: NOT PAIRED + try again");
  tap_obj(retry);
  check(page == P_LIST, "TRY AGAIN: back to the list");

  tap_obj(rod_btn[0]); type_pin("482913"); tap_obj(keys[11]);
  fake_state = PAIR_OK; run_ms(500);
  save("p4_paired");
  check(page == P_OK, "right PIN: Paired");
  run_ms(3000);
  check(restarted == 1, "then the watch restarts into the normal screens");

  printf("LVGL memory: most ever used %d bytes\n", mem_peak);
  check(mem_peak < 30000, "pairing screens are small (the normal screens are not loaded)");
  printf("%s: pairing screens, %d checks, %d failed\n", failed ? "FAIL" : "PASS", checks, failed);
  return failed != 0;
}

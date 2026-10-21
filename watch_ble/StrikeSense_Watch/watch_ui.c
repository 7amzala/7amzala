// =====================================================
// STRIKESENSE WATCH SCREENS
//
// Round 466 x 466 AMOLED. Only what matters on the wrist:
//   live graph (knock + tilt, last 20 s), the rod's state, and the
//   buttons to answer (REEL IN, I'M WATCHING, READY, SKIP).
// The outer ring: cyan = all fine, orange = something building, red = strike.
//
// Swipes:  left = SEA STATE, right = RELEARN THE SEA (hold 2 s); the
// opposite swipe goes back to the main page.  Down (any page) = LAST
// STRIKE (from the rod, fresh after a rod restart), up = back.  Up (any
// page) = BATTERY (rod + watch, time left), down = back.
// A strike always jumps to the main page; after 30 s on another page the
// watch goes back by itself.
//
// Screen: on while you look at the watch (watch_imu.cpp), off as soon as
// you look away (AMOLED black = almost no power). Also on: while a slow
// pull / drop-back builds, during a strike until you answer it, in the
// pull test, 10 s after a touch (that touch only wakes it). A knock only
// vibrates. Without the motion sensor: dark after 30 s, as before.
// While dark nothing is drawn and the chip runs at 80 MHz (watch_power.cpp).
//
// STRIKE screen: what the fish does now - RUN (pulling the line), KNOCK,
// SLACK (line went loose), QUIET (and for how long) - with a trend arrow
// (stronger / weaker), a strength ring, PULL % and TILT, the live graph with
// the strike marked, and a 30 s story bar. Each state has its own vibration;
// unanswered: a reminder every 30 s.
//
// Wrist vibration (buzz.c, if the Haptic Bzzz is wired): knock = tick,
// slow pull = pulses (faster as it grows), drop-back = 2 ticks, strike =
// strong buzz every 1.5 s until a button is pressed, rod lost = 3 buzzes.
//
// Look ("pro" theme): black, one thin gauge ring, a coloured state pill at
// the top, big numbers with a small unit, the graph in a dark rounded panel,
// round icon buttons. Same look on every page.
//
// Data comes from the rod over Bluetooth (ble_link.cpp, "/watch").
// =====================================================

#include "watch_ui.h"
#include "lvgl.h"
#include "ble_link.h"
#include "buzz.h"
#include "watch_bat.h"
#include "watch_imu.h"
#include "watch_power.h"

void lcd_set_brightness(uint8_t level);   // lcd_bsp.c
#include "strikesense_boot_fixed.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// ---------- fonts: bigger ones if they are switched on in lv_conf.h ----------

#if LV_FONT_MONTSERRAT_48
  #define FONT_HUGE (&lv_font_montserrat_48)
#elif LV_FONT_MONTSERRAT_36
  #define FONT_HUGE (&lv_font_montserrat_36)
#elif LV_FONT_MONTSERRAT_28
  #define FONT_HUGE (&lv_font_montserrat_28)
#else
  #define FONT_HUGE (&lv_font_montserrat_20)
#endif

#if LV_FONT_MONTSERRAT_36
  #define FONT_BIG (&lv_font_montserrat_36)
#elif LV_FONT_MONTSERRAT_28
  #define FONT_BIG (&lv_font_montserrat_28)
#else
  #define FONT_BIG (&lv_font_montserrat_20)
#endif

#if LV_FONT_MONTSERRAT_28
  #define FONT_MID (&lv_font_montserrat_28)
#else
  #define FONT_MID (&lv_font_montserrat_20)
#endif

#define FONT_SMALL (&lv_font_montserrat_20)
#define FONT_TINY  (&lv_font_montserrat_16)
#if LV_FONT_MONTSERRAT_14
  #define FONT_MICRO (&lv_font_montserrat_14)
#else
  #define FONT_MICRO (&lv_font_montserrat_16)
#endif

// ---------- colours (same as the dashboard) ----------

#define COL_BG      lv_color_hex(0x000000)
#define COL_PANEL   lv_color_hex(0x0a0e16)
#define COL_RING_BG lv_color_hex(0x161b26)
#define COL_CYAN    lv_color_hex(0x22d3ff)
#define COL_BLUE    lv_color_hex(0x1e7bff)
#define COL_VIOLET  lv_color_hex(0x9b8cff)
#define COL_ORANGE  lv_color_hex(0xffba52)
#define COL_RED     lv_color_hex(0xff4d5e)
#define COL_TEXT    lv_color_hex(0xeaf3ff)
#define COL_DIM     lv_color_hex(0x8ea6c8)
#define COL_DARK    lv_color_hex(0x5f7799)
#define COL_BTN     lv_color_hex(0x13213f)
#define COL_CARD    lv_color_hex(0x141924)    // list cells, round buttons
#define COL_LINE    lv_color_hex(0x232a3a)    // thin borders
#define COL_OFF     lv_color_hex(0x262b36)    // empty bars
#define COL_GREEN   lv_color_hex(0x3ddc84)

// Slow pull / drop-back shown (orange ring, title) from this % (was 30)
#define PULL_SHOW   10

#define SCREEN_IDLE_MS  30000      // no motion sensor: dark after this with nothing happening
#define SCREEN_TOUCH_MS 10000      // a touch: on this long (watch lying on a table)
#define SCREEN_KNOCK_MS 5000       // no motion sensor only: a knock turns it on this long
#define SCREEN_LOST_MS  5000       // rod lost: on this long

#define SWIPE_PX     70           // finger moved this far sideways = swipe
#define PAGE_BACK_MS 30000        // other page: back to the main page after this
#define HOLD_MS      2000         // RELEARN: hold this long

#define GRAPH_POINTS 200          // 200 x 100 ms = 20 s
#define KNOCK_LIMIT  100          // knock limit drawn at 100 on the graph (knock: top half, tilt: bottom half)
#define BOOT_MS      2500

typedef enum
{
  MODE_BOOT,
  MODE_LOOKING,
  MODE_LIVE,
  MODE_STRIKE,
  MODE_REELED,
  MODE_PULLTEST,
  MODE_SEA,
  MODE_RELEARN,
  MODE_BATTERY,
  MODE_LASTSTRIKE
} ui_mode_t;

static ui_mode_t mode = MODE_BOOT;
static uint32_t started_ms;

static lv_obj_t *scr;
static lv_obj_t *boot_img;
static lv_obj_t *ring;
static lv_obj_t *bars[4];
static lv_obj_t *bat_label;
static lv_obj_t *title;
static lv_obj_t *subtitle;
static lv_obj_t *chart;
static lv_chart_series_t *ser_knock;
static lv_chart_series_t *ser_limit;
static lv_chart_series_t *ser_tilt;
static lv_obj_t *btn1;                // big round button: READY, SKIP
static lv_obj_t *btn1_label;
static lv_obj_t *btn1_cap;            // its name under it
static lv_obj_t *dots[3];
static lv_obj_t *hint;

// looking for the rod: radar
static lv_obj_t *radar[3];
static lv_obj_t *radar_dot;
static lv_obj_t *radar_arc;

// pull test: a small rod that bends as you pull
static lv_obj_t *pt_rod;
static lv_obj_t *pt_line;
static lv_point_t pt_rod_pts[2] = { { 150, 62 }, { 300, 18 } };
static lv_point_t pt_line_pts[2] = { { 300, 18 }, { 318, 62 } };

// sea state: two small gauges with an icon
static lv_obj_t *sea_arc[2];
static lv_obj_t *sea_icon[2][3];
static lv_obj_t *sea_val[2];
static lv_obj_t *sea_cap[2];

// setup: three list cells
static lv_obj_t *cell[3];
static lv_obj_t *cell_fill[3];         // fills up while held
static lv_obj_t *cell_ico[3];
static lv_obj_t *cell_name[3];
static lv_obj_t *cell_sub[3];
static lv_obj_t *cell_arrow[3];

// pages: left/right: -1 relearn, 0 main, 1 sea state
//        up/down:    -1 battery, 0 (the left/right page), 1 last strike
static int page = 0;
static int vpage = 0;

// battery page
static lv_obj_t *bat_cap[2];
static lv_obj_t *bat_pct[2];
static lv_obj_t *bat_dot[2];
static lv_obj_t *bat_arc[2];          // outer = rod, inner = watch
static lv_obj_t *bat_left[2];
static lv_obj_t *bat_leftcap[2];

// last strike page
static lv_obj_t *ls_chart;
static lv_chart_series_t *ls_knock;
static lv_chart_series_t *ls_limit;
static lv_chart_series_t *ls_tilt;
static lv_obj_t *ls_mark;             // red line: the strike moment
static lv_obj_t *ls_mark_dot;
static lv_obj_t *ls_info[3];
static lv_obj_t *ls_story;
static uint32_t ls_shown_ms = 0;      // which answer is drawn
static uint32_t ls_asked = 0;
static uint32_t ls_entered = 0;
static long ls_strikeid = -1;
static int ls_view = 0;               // 0 = ZOOM (2 s + 2 s), 1 = 30 s overview; tap the graph to switch
static uint32_t page_since = 0;
static bool swiped = false;           // this touch was a swipe: no button press
static uint32_t hold_from = 0;

// vibration
static long strike_seen = 0;          // strike id on the screen now
static long strike_quiet = -1;        // strike id answered with a button: stop buzzing
static uint32_t buzz_strike_at = 0;
static uint32_t buzz_pull_at = 0;
static bool drop_was = false;
static bool was_connected = false;

// ---------- what the fish does (from the live data, every 100 ms) ----------
typedef enum { FISH_QUIET, FISH_KNOCK, FISH_SLACK, FISH_RUN } fish_t;

#define STORY_CELLS 30                // 30 s, one cell per second

static fish_t fish = FISH_QUIET;
static fish_t fish_was = FISH_QUIET;
static uint32_t fish_run_ms = 0;      // last time it pulled / slacked / knocked
static uint32_t fish_slack_ms = 0;
static uint32_t fish_knock_ms = 0;
static uint32_t fish_quiet_since = 0;
static float fish_fast = 0;           // strength, quick average
static float fish_slow = 0;           // strength, slow average (for the trend)
static int fish_trend = 0;            // +1 stronger, -1 weaker, 0 steady
static fish_t story[STORY_CELLS];
static int story_head = 0;
static fish_t story_cell = FISH_QUIET;
static uint32_t story_at = 0;
static uint32_t strike_ms = 0;        // when the strike on the screen started
static long strike_ms_id = -1;
static uint32_t buzz_state_at = 0;
static uint32_t buzz_remind_at = 0;

// STRIKE screen objects
static lv_obj_t *ring_glow;
static lv_obj_t *st_time;
static lv_obj_t *st_pill;
static lv_obj_t *st_dot;
static lv_obj_t *st_state;
static lv_obj_t *st_trend;
static lv_obj_t *st_val[2];
static lv_obj_t *st_unit[2];
static lv_obj_t *st_cap[2];
static lv_obj_t *st_panel;
static lv_obj_t *st_bars;
static lv_obj_t *st_lines;
static lv_chart_series_t *st_bar_ser;
static lv_chart_series_t *st_knock_ser;
static lv_chart_series_t *st_tilt_ser;
static lv_obj_t *st_mark;
static lv_obj_t *st_mark_dot;
static lv_obj_t *st_now;
static lv_obj_t *st_story;
static lv_obj_t *st_btn[2];
static lv_obj_t *st_btn_icon[2];
static lv_obj_t *st_btn_cap[2];

// screen on / dark
static bool screen_on = true;
static uint32_t screen_until = 0;

static void screen_wake(uint32_t ms)
{
  uint32_t until = lv_tick_get() + ms;

  if((int32_t)(until - screen_until) > 0)
    screen_until = until;

  if(!screen_on)
  {
    screen_on = true;
    power_screen(true);                              // full speed first
    lv_disp_enable_invalidation(NULL, true);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);                               // draw the current screen...
    lcd_set_brightness(255);                         // ...then show it
  }
}

static void screen_dark(void)
{
  screen_on = false;
  lcd_set_brightness(0);
  lv_disp_enable_invalidation(NULL, false);           // nothing drawn while dark
  power_screen(false);
}

static const char *btn1_path = "";
static lv_obj_t *hold_btn = NULL;     // the hold-2-s cell being held

static long last_knock_id = -1;
static uint32_t knock_until = 0;

// ---------- small helpers ----------

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t colour)
{
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, colour, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, "");
  return l;
}

static void show(lv_obj_t *o, bool on)
{
  if(lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == !on)
    return;                       // already so: no redraw

  if(on)
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void set_text(lv_obj_t *l, const char *t)
{
  if(strcmp(lv_label_get_text(l), t) != 0)
    lv_label_set_text(l, t);
}

// Setting a style always redraws, even with the same value. These only
// touch LVGL when something changed, so the screen is not redrawn 10x a
// second for nothing (that made the watch lag behind the rod).
static void set_text_color(lv_obj_t *o, lv_color_t c)
{
  if(lv_obj_get_style_text_color(o, 0).full != c.full)
    lv_obj_set_style_text_color(o, c, 0);
}

static void set_bg_color(lv_obj_t *o, lv_color_t c)
{
  if(lv_obj_get_style_bg_color(o, 0).full != c.full)
    lv_obj_set_style_bg_color(o, c, 0);
}

static void set_font(lv_obj_t *o, const lv_font_t *f)
{
  if(lv_obj_get_style_text_font(o, 0) != f)
    lv_obj_set_style_text_font(o, f, 0);
}

static void ls_chart_clicked(lv_event_t *e)
{
  (void)e;

  if(swiped)
    return;

  ls_view = 1 - ls_view;
  ls_shown_ms = 0;                    // draw again
  buzz_play(BUZZ_TAP);
}

static void button_clicked(lv_event_t *e)
{
  const char *path = (const char *)lv_event_get_user_data(e);

  if(path != NULL && strcmp(path, "#teach") == 0 && !swiped)
  {
    imu_teach_start();              // TEACH MY LOOK (not sent to the rod)
    buzz_play(BUZZ_TAP);
    return;
  }

  if(path != NULL && path[0] != 0 && !swiped)
  {
    ble_link_command(path);
    buzz_play(BUZZ_TAP);

    if(mode == MODE_STRIKE)
      strike_quiet = strike_seen;     // answered: stop the strike buzz
  }
}

static lv_obj_t *button(lv_obj_t **text)
{
  lv_obj_t *b = lv_btn_create(scr);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);

  *text = label(b, FONT_SMALL, COL_TEXT);
  lv_obj_center(*text);

  return b;
}

// a plain rectangle / circle (no theme style)
static lv_obj_t *box(lv_obj_t *parent, int w, int h, int radius, lv_color_t c)
{
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

// a gauge arc (270 degrees, gap at the bottom)
static lv_obj_t *gauge_arc(lv_obj_t *parent, int size, int width, lv_color_t track)
{
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_set_size(a, size, size);
  lv_arc_set_range(a, 0, 100);
  lv_arc_set_bg_angles(a, 135, 45);
  lv_arc_set_rotation(a, 0);
  lv_arc_set_value(a, 0);
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(a, 0, 0);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, track, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
  return a;
}

// Story bars (30 cells, one colour each) are drawn into a small picture:
// one object instead of 30 (the watch has only 48 KB for the screens).
#define STORY_W 334
#define STORY_H 6

static lv_color_t st_story_buf[STORY_W * STORY_H];
static lv_color_t ls_story_buf[STORY_W * STORY_H];
static signed char st_story_drawn[30];
static signed char ls_story_drawn[30];

static lv_obj_t *story_canvas(lv_color_t *buf, signed char *drawn, int y)
{
  lv_obj_t *cv = lv_canvas_create(scr);
  lv_canvas_set_buffer(cv, buf, STORY_W, STORY_H, LV_IMG_CF_TRUE_COLOR);
  lv_obj_align(cv, LV_ALIGN_TOP_MID, 0, y);
  memset(buf, 0, sizeof(lv_color_t) * STORY_W * STORY_H);          // black
  memset(drawn, -1, 30);
  return cv;
}

static void story_paint(lv_obj_t *cv, lv_color_t *buf, signed char *drawn, int i, lv_color_t c, int kind)
{
  if(drawn[i] == kind)
    return;                           // same as on the screen: no redraw

  drawn[i] = (signed char)kind;

  int x0 = i * STORY_W / 30;
  int x1 = (i + 1) * STORY_W / 30 - 1;     // 1 px gap between cells

  for(int y = 0; y < STORY_H; y++)
    for(int x = x0; x < x1; x++)
      buf[y * STORY_W + x] = c;

  lv_obj_invalidate(cv);
}

static void set_arc(lv_obj_t *a, int value, lv_color_t c)
{
  lv_arc_set_value(a, value < 0 ? 0 : (value > 100 ? 100 : value));   // no redraw if the same

  if(lv_obj_get_style_arc_color(a, LV_PART_INDICATOR).full != c.full)
    lv_obj_set_style_arc_color(a, c, LV_PART_INDICATOR);
}

// Place a button, its text, colours and what it sends
static void set_button(lv_obj_t *b, lv_obj_t *t, const char **slot, const char *text, const char *path,
                       int y, int w, int h, lv_color_t bg, lv_color_t fg)
{
  show(b, true);
  show(btn1_cap, b == btn1);

  if(lv_obj_get_style_width(b, 0) != w || lv_obj_get_style_height(b, 0) != h ||
     lv_obj_get_style_y(b, 0) != y - h / 2)
  {
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y - h / 2);
  }

  set_bg_color(b, bg);
  set_text_color(t, fg);
  set_text(t, text);

  if(*slot != path)
  {
    *slot = path;
    lv_obj_remove_event_cb(b, button_clicked);
    lv_obj_add_event_cb(b, button_clicked, LV_EVENT_CLICKED, (void *)path);
  }
}

static uint32_t spin = 0;

// the outer gauge ring and its soft glow
static void ring_set(int value, lv_color_t colour)
{
  set_arc(ring, value, colour);
  set_arc(ring_glow, value, colour);
}

// the state pill at the top: [dot] TEXT, tinted in its colour
static void pill_set(const char *text, lv_color_t c, bool dot)
{
  set_text(st_state, text);
  set_text_color(st_state, c);
  set_bg_color(st_dot, c);
  set_bg_color(st_pill, c);
  show(st_dot, dot);

  if(lv_obj_get_style_border_color(st_pill, 0).full != c.full)
    lv_obj_set_style_border_color(st_pill, c, 0);
}

// a big number with its unit right after it and a caption under it.
// i = 0 left, 1 right; the number + unit stay centred on their column.
static void metric(int i, const char *val, const char *unit, const char *cap, lv_color_t c, int y)
{
  set_text_color(st_val[i], c);
  set_text_color(st_unit[i], c);
  set_text(st_cap[i], cap);

  bool moved = lv_obj_get_style_y(st_val[i], 0) != y;

  if(strcmp(lv_label_get_text(st_val[i]), val) == 0 && strcmp(lv_label_get_text(st_unit[i]), unit) == 0 && !moved)
    return;

  set_text(st_val[i], val);
  set_text(st_unit[i], unit);
  lv_obj_update_layout(st_val[i]);
  lv_obj_update_layout(st_unit[i]);

  lv_coord_t vw = lv_obj_get_width(st_val[i]);
  lv_coord_t w = vw + (unit[0] ? 4 + lv_obj_get_width(st_unit[i]) : 0);
  lv_coord_t cx = i == 0 ? -73 : 73;
  lv_obj_align(st_val[i], LV_ALIGN_TOP_MID, cx - w / 2 + vw / 2, y);
  lv_obj_align_to(st_unit[i], st_val[i], LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);
  lv_obj_align(st_cap[i], LV_ALIGN_TOP_MID, cx, y + 40);
}

// "now" dot at the end of the tilt line in the graph panel
static void graph_now(void)
{
  lv_coord_t *ty = lv_chart_get_y_array(chart, ser_tilt);
  int last = (lv_chart_get_x_start_point(chart, ser_tilt) + GRAPH_POINTS - 1) % GRAPH_POINTS;
  int tv = ty[last] == LV_CHART_POINT_NONE ? 0 : ty[last];
  lv_obj_set_y(st_now, 10 + (900 - tv) * (126 - 20) / 1200 - 6);
}

static const char *wave_name(int w)
{
  static const char *n[] = { "flat", "small", "medium", "big" };
  return n[w < 0 ? 0 : (w > 3 ? 3 : w)];
}

static const char *wind_name(int w)
{
  static const char *n[] = { "calm", "normal", "strong", "very strong" };
  return n[w < 0 ? 0 : (w > 3 ? 3 : w)];
}

// ---------- layouts ----------

static void place(lv_obj_t *o, const lv_font_t *f, int y)
{
  lv_obj_set_style_text_font(o, f, 0);
  lv_obj_align(o, LV_ALIGN_TOP_MID, 0, y);
}

static void layout(ui_mode_t m)
{
  if(m == mode)
    return;

  mode = m;

  bool live = m == MODE_LIVE;
  bool st = m == MODE_STRIKE;
  bool sea = m == MODE_SEA;
  bool bat = m == MODE_BATTERY;
  bool lst = m == MODE_LASTSTRIKE;
  bool look = m == MODE_LOOKING;

  show(boot_img, m == MODE_BOOT);

  // looking: radar
  for(int i = 0; i < 3; i++)
    show(radar[i], look);

  show(radar_dot, look);
  show(radar_arc, look);

  // the outer gauge ring: home, strike, reeled in, pull test, setup
  show(ring, live || st || m == MODE_REELED || m == MODE_PULLTEST || m == MODE_RELEARN);
  show(ring_glow, live || st || m == MODE_PULLTEST);
  lv_obj_set_style_arc_width(ring, st || m == MODE_PULLTEST ? 12 : 10, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(ring, st || m == MODE_PULLTEST ? 12 : 10, LV_PART_MAIN);

  // top: signal + rod battery (the strike screen shows the time there)
  bool top = live || sea || lst || m == MODE_REELED || m == MODE_PULLTEST || m == MODE_RELEARN;

  show(bat_label, top);

  for(int i = 0; i < 4; i++)
    show(bars[i], top);

  // page dots: only on the three side-by-side pages
  for(int i = 0; i < 3; i++)
    show(dots[i], live || sea || m == MODE_RELEARN);

  // state pill: everywhere but the radar, boot and battery
  show(st_pill, !look && !bat && m != MODE_BOOT);
  lv_obj_align(st_pill, LV_ALIGN_TOP_MID, st ? -10 : 0, 66);
  show(st_trend, st);
  show(st_time, st);

  // graph panel: home and strike (same live data)
  show(st_panel, live || st);
  show(st_mark, false);
  show(st_mark_dot, false);
  show(st_story, st);

  // big numbers: home, strike, last strike
  for(int i = 0; i < 2; i++)
  {
    show(st_val[i], live || st || lst);
    show(st_unit[i], live || st || lst);
    show(st_cap[i], live || st || lst);
    show(st_btn[i], st);
    show(st_btn_cap[i], st);
  }

  // REEL IN: round button, on its own in the middle at home, right on the strike screen
  if(live)
  {
    show(st_btn[1], true);
    show(st_btn_cap[1], true);
    lv_obj_align(st_btn[1], LV_ALIGN_TOP_MID, 0, 330);
    lv_obj_align(st_btn_cap[1], LV_ALIGN_TOP_MID, 0, 398);
  }
  else
  {
    lv_obj_align(st_btn[1], LV_ALIGN_TOP_MID, 57, 322);
    lv_obj_align(st_btn_cap[1], LV_ALIGN_TOP_MID, 57, 392);
  }

  show(btn1, false);
  show(btn1_cap, false);

  // pull test rod picture
  show(pt_rod, m == MODE_PULLTEST);
  show(pt_line, m == MODE_PULLTEST);

  // sea state
  for(int r = 0; r < 2; r++)
  {
    show(sea_arc[r], sea);
    show(sea_val[r], sea);
    show(sea_cap[r], sea);

    for(int i = 0; i < 3; i++)
      show(sea_icon[r][i], sea && (r == 1 || i < 2));      // 2 waves, 3 wind lines
  }

  // setup cells
  for(int i = 0; i < 3; i++)
    show(cell[i], false);

  // battery
  for(int r = 0; r < 2; r++)
  {
    show(bat_arc[r], bat);
    show(bat_cap[r], bat);
    show(bat_pct[r], bat);
    show(bat_dot[r], bat);
    show(bat_left[r], bat);
    show(bat_leftcap[r], bat);
  }

  // last strike
  show(ls_chart, lst);
  show(ls_story, lst);

  for(int i = 0; i < 3; i++)
    show(ls_info[i], lst);

  // texts: which page uses the title / subtitle / hint
  show(title, look || m == MODE_REELED || m == MODE_PULLTEST);
  show(subtitle, look || live || m == MODE_REELED || m == MODE_PULLTEST || lst);
  show(hint, m == MODE_PULLTEST || sea);

  lv_obj_set_style_bg_color(scr, COL_BG, 0);

  switch(m)
  {
    case MODE_LOOKING:
      place(title, FONT_SMALL, 330);
      place(subtitle, FONT_MICRO, 360);
      set_text_color(subtitle, COL_DIM);
      break;

    case MODE_LIVE:
      place(subtitle, FONT_MICRO, 306);
      break;

    case MODE_REELED:
      place(title, FONT_MID, 110);
      place(subtitle, FONT_SMALL, 148);
      lv_obj_set_style_text_font(btn1_label, FONT_BIG, 0);
      lv_obj_set_style_border_width(btn1, 0, 0);
      lv_obj_set_style_outline_width(btn1, 10, 0);       // soft halo (a shadow needs too much memory)
      lv_obj_set_style_outline_pad(btn1, 2, 0);
      lv_obj_set_style_outline_color(btn1, COL_CYAN, 0);
      lv_obj_set_style_outline_opa(btn1, LV_OPA_20, 0);
      lv_obj_align(btn1_cap, LV_ALIGN_TOP_MID, 0, 336);
      set_text_color(btn1_cap, COL_CYAN);
      set_font(btn1_cap, FONT_TINY);
      break;

    case MODE_PULLTEST:
      place(title, FONT_HUGE, 104);
      place(subtitle, FONT_TINY, 160);
      place(hint, FONT_MICRO, 276);
      lv_obj_set_style_text_font(btn1_label, FONT_SMALL, 0);
      lv_obj_set_style_border_width(btn1, 2, 0);
      lv_obj_set_style_border_color(btn1, lv_color_hex(0x4a5872), 0);
      lv_obj_set_style_outline_width(btn1, 0, 0);
      lv_obj_align(btn1_cap, LV_ALIGN_TOP_MID, 0, 384);
      set_text_color(btn1_cap, COL_DIM);
      set_font(btn1_cap, FONT_MICRO);
      break;

    case MODE_SEA:
      place(hint, FONT_MICRO, 330);
      break;

    case MODE_RELEARN:
      place(title, FONT_BIG, 150);
      place(subtitle, FONT_SMALL, 206);
      break;

    case MODE_LASTSTRIKE:
      place(subtitle, FONT_TINY, 98);
      break;

    default:
      break;
  }
}

// ---------- every 100 ms: state, texts, colours ----------

static void top_icons(const ble_link_status_t *s)
{
  int n = s->rssi == 0 ? 0 : s->rssi >= -60 ? 4 : s->rssi >= -70 ? 3 : s->rssi >= -80 ? 2 : 1;

  for(int i = 0; i < 4; i++)
    set_bg_color(bars[i], i < n ? COL_CYAN : COL_OFF);

  char t[24];

  if(s->bat < 0)
    snprintf(t, sizeof(t), "--");
  else
    snprintf(t, sizeof(t), "%d%%", s->bat);

  set_text(bat_label, t);
  set_text_color(bat_label, s->bat >= 0 && s->bat < 15 ? COL_RED : COL_DIM);
}

// ---------- swipe: watch the finger (also over buttons) ----------

static void swipe_timer(lv_timer_t *t)
{
  (void)t;

  static bool down = false;
  static lv_point_t from;

  lv_indev_t *in = lv_indev_get_next(NULL);

  if(in == NULL)
    return;

  lv_point_t p;
  lv_indev_get_point(in, &p);
  bool pressed = in->proc.state == LV_INDEV_STATE_PRESSED;

  if(pressed && !down)
  {
    down = true;
    from = p;
    swiped = !screen_on;            // dark: this touch only wakes the screen
    screen_wake(imu_present() ? SCREEN_TOUCH_MS : SCREEN_IDLE_MS);
    return;
  }

  if(!pressed)
  {
    down = false;
    return;
  }

  int dx = p.x - from.x;
  int dy = p.y - from.y;
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;

  if(swiped || (adx < SWIPE_PX && ady < SWIPE_PX))
    return;

  swiped = true;                    // one page per swipe
  hold_from = 0;

  if(mode == MODE_BOOT || mode == MODE_LOOKING || mode == MODE_STRIKE)
    return;

  int before = page;
  int vbefore = vpage;

  if(adx >= ady)
  {
    // left / right: only on the normal pages
    if(vpage == 0)
    {
      if(dx < 0 && page < 1)
        page++;                     // left: main -> sea state, relearn -> main
      else if(dx > 0 && page > -1)
        page--;                     // right: main -> relearn, sea state -> main
    }
  }
  else if(dy < 0)
  {
    // finger up: battery, or back from the last strike
    if(vpage == 0)
      vpage = -1;
    else if(vpage == 1)
      vpage = 0;
  }
  else
  {
    // finger down: last strike, or back from the battery
    if(vpage == 0)
      vpage = 1;
    else if(vpage == -1)
      vpage = 0;
  }

  if(vpage == 1 && vbefore != 1)
  {
    ls_view = 0;                      // ZOOM first
    ls_shown_ms = 0;
    ble_link_ask_last_strike();
    ls_asked = lv_tick_get();
    ls_entered = ls_asked;
  }

  if(page != before || vpage != vbefore)
    buzz_play(BUZZ_TAP);

  page_since = lv_tick_get();
}

static void page_dots(void)
{
  for(int i = 0; i < 3; i++)
    set_bg_color(dots[i], i == page + 1 ? COL_TEXT : lv_color_hex(0x3a4458));
}

static void sea_row(int r, const char *name, int level, lv_color_t c)
{
  set_text(sea_val[r], name);
  set_font(sea_val[r], strlen(name) > 7 ? FONT_TINY : FONT_SMALL);    // "very strong" fits
  set_arc(sea_arc[r], (level + 1) * 25, c);
}

static void sea_page(const ble_link_status_t *s)
{
  layout(MODE_SEA);

  pill_set("SEA STATE", COL_CYAN, false);
  set_text(hint, "measured by the rod");
  set_text_color(hint, COL_DIM);

  sea_row(0, wave_name(s->wave), s->wave, COL_CYAN);
  sea_row(1, wind_name(s->wind), s->wind, COL_VIOLET);
}

static uint32_t teach_shown = 0;       // when the teach result appeared

// one setup cell: name, small text under it, how far it is held (0..1)
static void cell_set(int i, const char *sub, float held, bool busy)
{
  bool h = held > 0;

  set_text(cell_sub[i], busy ? "..." : h ? "keep holding..." : sub);
  set_text_color(cell_sub[i], h ? COL_ORANGE : COL_DARK);

  if(lv_obj_get_style_border_color(cell[i], 0).full != (h ? COL_ORANGE : COL_LINE).full)
    lv_obj_set_style_border_color(cell[i], h ? COL_ORANGE : COL_LINE, 0);

  int w = (int)(held * 312);
  show(cell_fill[i], w > 0);

  if(w > 0 && lv_obj_get_style_width(cell_fill[i], 0) != w)
    lv_obj_set_width(cell_fill[i], w);
}

static void relearn_page(const ble_link_status_t *s, uint32_t now)
{
  layout(MODE_RELEARN);

  // teaching the looking angle: show only that
  int ts = imu_teach_state();

  if(ts == 1)
    teach_shown = 0;
  else if((ts == 2 || ts == 3) && teach_shown == 0)
    teach_shown = now;                // the result: shown for 2.5 s

  bool result = (ts == 2 || ts == 3) && now - teach_shown < 2500;
  bool teach = ts == 1 || result;

  show(title, teach);
  show(subtitle, teach);

  for(int i = 0; i < 3; i++)
    show(cell[i], !teach && (i < 2 || imu_present()));

  if(teach)
  {
    lv_color_t c = ts == 3 ? COL_ORANGE : COL_CYAN;
    pill_set("TEACH MY LOOK", c, true);
    set_text(title, ts == 1 ? "Hold still" : ts == 2 ? "Learned" : "Moved");
    set_text_color(title, c);
    set_text(subtitle, ts == 1 ? "hold the watch the way\nyou look at it" : ts == 2 ? "your looking angle\nis saved" : "try again,\nhold it still");
    set_text_color(subtitle, COL_DIM);
    show(ring, true);
    ring_set((int)(imu_teach_progress() * 100), c);
    return;
  }

  pill_set("SETUP", COL_ORANGE, false);

  // RELEARN THE SEA and NEW PULL DIRECTION: hold 2 s (no accidents)
  lv_obj_t *held_now = NULL;

  if(!swiped)
  {
    if(lv_obj_has_state(cell[0], LV_STATE_PRESSED))
      held_now = cell[0];
    else if(lv_obj_has_state(cell[1], LV_STATE_PRESSED))
      held_now = cell[1];
  }

  if(held_now != hold_btn)
  {
    hold_btn = held_now;
    hold_from = held_now ? now : 0;
  }

  uint32_t held = hold_btn ? now - hold_from : 0;

  if(held >= HOLD_MS)
  {
    bool sea = hold_btn == cell[0];
    ble_link_command(sea ? "/relearn" : "/teachdir");
    buzz_play(BUZZ_DROP);             // two ticks: done
    hold_btn = NULL;
    hold_from = 0;
    swiped = true;                    // this touch is used up: letting go presses nothing
    page = 0;                         // back to the main page: LEARNING or PULL TEST
    vpage = 0;
    return;
  }

  // the ring only shows while a cell is held
  show(ring, hold_btn != NULL);
  ring_set((int)(held * 100 / HOLD_MS), COL_ORANGE);

  float f = (float)held / HOLD_MS;

  cell_set(0, "hold 2 s", hold_btn == cell[0] ? f : 0, s->command_busy);
  cell_set(1, "hold 2 s", hold_btn == cell[1] ? f : 0, false);
  cell_set(2, "tap", 0, false);
}

// ---------- STRIKE screen ----------

static lv_color_t fish_color(fish_t f)
{
  return f == FISH_RUN ? COL_RED : f == FISH_KNOCK ? COL_ORANGE : f == FISH_SLACK ? COL_VIOLET : COL_CYAN;
}

// Every 100 ms (graph_timer): what the fish does, how strong, the trend, the story
static void fish_track(const ble_link_status_t *s, int k)
{
  uint32_t now = lv_tick_get();
  float allow = s->allow > 0.2f ? s->allow : 0.2f;

  if(s->bucket >= PULL_SHOW || s->tilt > allow)
    fish_run_ms = now;                          // pulling the line beyond the waves

  if(s->dropwatch || s->drop >= PULL_SHOW || s->tilt < -allow)
    fish_slack_ms = now;                        // line went loose

  if(k >= KNOCK_LIMIT)
    fish_knock_ms = now;

  fish_t f = FISH_QUIET;

  if(fish_run_ms && now - fish_run_ms < 1500)
    f = FISH_RUN;
  else if(fish_slack_ms && now - fish_slack_ms < 1500)
    f = FISH_SLACK;
  else if(fish_knock_ms && now - fish_knock_ms < 3000)
    f = FISH_KNOCK;

  if(f != FISH_QUIET)
    fish_quiet_since = 0;
  else if(fish_quiet_since == 0)
    fish_quiet_since = now;

  fish = f;

  // strength 0..1: pull %, tilt beyond the waves, knock size - the biggest
  float st = s->bucket / 100.0f;
  float tl = fabsf(s->tilt) / (allow * 3);
  float kk = k / 300.0f;

  if(tl > st) st = tl;
  if(kk > st) st = kk;
  if(st > 1) st = 1;

  fish_fast += (st - fish_fast) * 0.3f;
  fish_slow += (st - fish_slow) * 0.04f;
  fish_trend = fish_fast > fish_slow + 0.08f ? 1 : (fish_fast < fish_slow - 0.08f ? -1 : 0);

  // story: the strongest state of each second, last 30 s
  if(f > story_cell)
    story_cell = f;

  if(now - story_at >= 1000)
  {
    story[story_head] = story_cell;
    story_head = (story_head + 1) % STORY_CELLS;
    story_cell = f;
    story_at = now;
  }
}

static void strike_page(const ble_link_status_t *s, uint32_t now)
{
  layout(MODE_STRIKE);
  set_bg_color(scr, COL_BG);

  if(s->strikeid != strike_ms_id)
  {
    strike_ms_id = s->strikeid;
    strike_ms = now;
  }

  lv_color_t c = fish_color(fish);
  int strength = (int)(fish_fast * 100);

  // strength ring + soft glow
  ring_set(strength < 3 ? 3 : strength, c);

  // time since the strike
  char a[32];
  uint32_t secs = (now - strike_ms) / 1000;
  snprintf(a, sizeof(a), "%lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
  set_text(st_time, a);

  // state pill: RUN / KNOCK / SLACK / QUIET m:ss
  if(fish == FISH_QUIET)
  {
    uint32_t q = fish_quiet_since ? (now - fish_quiet_since) / 1000 : 0;
    snprintf(a, sizeof(a), "QUIET  %lu:%02lu", (unsigned long)(q / 60), (unsigned long)(q % 60));
  }
  else
  {
    snprintf(a, sizeof(a), "%s", fish == FISH_RUN ? "RUN" : fish == FISH_KNOCK ? "KNOCK" : "SLACK");
  }

  pill_set(a, c, true);

  // trend arrow next to the pill
  set_text(st_trend, fish_trend > 0 ? LV_SYMBOL_UP : fish_trend < 0 ? LV_SYMBOL_DOWN : "");
  set_text_color(st_trend, fish_trend > 0 ? COL_ORANGE : COL_CYAN);
  lv_obj_update_layout(st_pill);
  lv_obj_align_to(st_trend, st_pill, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

  // PULL % and TILT, each number centred with its unit right after it
  snprintf(a, sizeof(a), "%d", s->bucket);
  metric(0, a, "%", "PULL", COL_TEXT, 104);
  snprintf(a, sizeof(a), "%+.1f", s->tilt);
  metric(1, a, "deg", "TILT", COL_VIOLET, 104);

  // strike marker on the graph (100 ms per point)
  int idx = GRAPH_POINTS - 1 - (int)((now - strike_ms) / 100);
  show(st_mark, idx >= 0);
  show(st_mark_dot, idx >= 0);

  if(idx >= 0)
  {
    int x = 10 + idx * (354 - 20) / (GRAPH_POINTS - 1);
    lv_obj_set_x(st_mark, x - 1);
    lv_obj_set_x(st_mark_dot, x - 5);
  }

  graph_now();

  // story bar: the last 30 s, oldest left
  for(int i = 0; i < STORY_CELLS; i++)
  {
    fish_t f = story[(story_head + i) % STORY_CELLS];
    story_paint(st_story, st_story_buf, st_story_drawn, i, f == FISH_QUIET ? lv_color_hex(0x343a4a) : fish_color(f), f);
  }

  set_text(st_btn_cap[0], s->command_busy ? "..." : "WATCHING");
  set_text(st_btn_cap[1], "REEL IN");
}

static void time_left(char *out, size_t size, int pct, long minutes)
{
  if(pct < 0)
    snprintf(out, size, "--");
  else if(minutes < 0)
    snprintf(out, size, "measuring");
  else
    snprintf(out, size, "%ldh %02ldm", minutes / 60, minutes % 60);
}

static void battery_row(int r, int pct, long minutes)
{
  char a[24];

  lv_color_t c = pct < 0 ? COL_DIM : pct < 20 ? COL_RED : pct < 50 ? COL_ORANGE : COL_GREEN;

  if(pct < 0)
    snprintf(a, sizeof(a), "--");
  else
    snprintf(a, sizeof(a), "%d%%", pct);

  set_text(bat_pct[r], a);
  set_bg_color(bat_dot[r], c);
  set_arc(bat_arc[r], pct < 0 ? 0 : pct, c);

  time_left(a, sizeof(a), pct, minutes);
  set_text(bat_left[r], a);
  set_text_color(bat_left[r], r == 0 ? COL_TEXT : c);
}

static void battery_page(const ble_link_status_t *s)
{
  layout(MODE_BATTERY);

  battery_row(0, s->bat, s->batleft);
  battery_row(1, watch_bat_percent(), watch_bat_left_min());
}

// the strongest thing in each part of a recording, like the strike story bar
static void ls_story_draw(const short *kk, const short *tt, int n, float allow)
{
  int lim = (int)(allow * 100);

  for(int c = 0; c < 30; c++)
  {
    fish_t f = FISH_QUIET;

    for(int i = c * n / 30; i < (c + 1) * n / 30 && i < n; i++)
    {
      fish_t g = tt[i] > lim ? FISH_RUN : tt[i] < -lim ? FISH_SLACK : kk[i] >= KNOCK_LIMIT ? FISH_KNOCK : FISH_QUIET;

      if(g > f)
        f = g;
    }

    story_paint(ls_story, ls_story_buf, ls_story_drawn, c, f == FISH_QUIET ? lv_color_hex(0x343a4a) : fish_color(f), f);
  }
}

static void last_strike_page(const ble_link_status_t *s, uint32_t now)
{
  layout(MODE_LASTSTRIKE);

  pill_set("LAST STRIKE", COL_RED, true);

  static ble_last_strike_t ls;        // big: not on the stack
  bool got = ble_link_get_last_strike(&ls);

  // ask again every 5 s, every 1 s while the strike is still recorded (the
  // rest comes in), every 1.5 s while nothing came; at once after a new strike
  uint32_t every = !got ? 1500u : (ls.recording ? 1000u : 5000u);

  if(now - ls_asked > every || s->strikeid != ls_strikeid)
  {
    ls_strikeid = s->strikeid;
    ls_asked = now;
    ble_link_ask_last_strike();
  }

  bool graph = got && !ls.error && ls.have && ls.n > 1;

  show(ls_chart, graph);
  show(ls_story, graph);

  for(int i = 0; i < 2; i++)
  {
    show(st_val[i], graph);
    show(st_unit[i], graph);
    show(st_cap[i], graph);
  }

  set_text_color(subtitle, COL_TEXT);

  if(!graph)
  {
    if(!got)
      set_text(subtitle, now - ls_entered > 6000 ? "the rod does not answer" : "loading...");
    else if(ls.error)
      set_text(subtitle, ls.error == 404 ? "update the rod sketch" : "the rod said no");
    else
      set_text(subtitle, "No strike yet");

    set_text(ls_info[0], "");
    set_text(ls_info[1], got && ls.error == 404 ? "(it has no last strike yet:\nupload the newest rod sketch)" :
                         got && !ls.error ? "since the rod was switched on" : "");
    set_text(ls_info[2], "");
    return;
  }

  char a[64];

  // how long ago, counting on from when it was fetched
  long ago = ls.ago_s + (long)((now - ls.got_ms) / 1000);
  const char *type = ls.type[0] ? ls.type : "STRIKE";

  if(ago < 60)
    snprintf(a, sizeof(a), "%s  -  %ld s ago", type, ago);
  else if(ago < 3600)
    snprintf(a, sizeof(a), "%s  -  %ld min ago", type, ago / 60);
  else
    snprintf(a, sizeof(a), "%s  -  %ld h %02ld min ago", type, ago / 3600, (ago % 3600) / 60);

  set_text(subtitle, a);

  // which view: ZOOM (50 a second around the strike) or the 30 s overview
  bool zoom = ls_view == 0 && ls.zn > 1;
  int n = zoom ? ls.zn : ls.n;
  int pre = zoom ? ls.zpre : ls.pre;
  const short *kk = zoom ? ls.zk : ls.k;
  const short *tt = zoom ? ls.zt : ls.t;

  // draw the graph once per answer (and when the view changes)
  if(ls.got_ms != ls_shown_ms)
  {
    ls_shown_ms = ls.got_ms;
    lv_chart_set_point_count(ls_chart, n);

    for(int i = 0; i < n; i++)
    {
      lv_chart_set_value_by_id(ls_chart, ls_knock, i, kk[i]);
      lv_chart_set_value_by_id(ls_chart, ls_limit, i, KNOCK_LIMIT);
      lv_chart_set_value_by_id(ls_chart, ls_tilt, i, tt[i]);
    }

    lv_chart_refresh(ls_chart);

    // red line + dot where the strike was (in the chart's drawing area)
    int x = n > 1 ? pre * (354 - 20) / (n - 1) : 0;
    lv_obj_set_pos(ls_mark, x - 1, 0);
    lv_obj_set_pos(ls_mark_dot, x - 5, -8);

    ls_story_draw(kk, tt, n, s->allow > 0.2f ? s->allow : 0.2f);
  }

  // the info line ("|" = the strike)
  if(zoom)
    snprintf(a, sizeof(a), "ZOOM  %.1f s | %.1f s   -   tap: 30 s", pre / 50.0f, (n - pre) / 50.0f);
  else
    snprintf(a, sizeof(a), "%d s | %d s   -   tap: zoom", pre / 5, (n - pre) / 5);

  set_text(ls_info[0], a);
  set_text(ls_info[1], "");

  snprintf(a, sizeof(a), "%+.1f", ls.tiltpeak);
  metric(0, a, "deg", "TILT PEAK", COL_VIOLET, 296);
  snprintf(a, sizeof(a), "%d", ls.pull);
  metric(1, a, "%", "PULL", COL_TEXT, 296);

  if(ls.recording)
  {
    set_text(ls_info[2], "recording - the rest comes in...");
    return;
  }

  snprintf(a, sizeof(a), "%d strike%s since the rod started", ls.count, ls.count == 1 ? "" : "s");
  set_text(ls_info[2], a);
}

static void ui_timer(lv_timer_t *t)
{
  (void)t;

  uint32_t now = lv_tick_get();
  ble_link_status_t s;
  ble_link_get(&s, false);

  if(now - started_ms < BOOT_MS)
  {
    layout(MODE_BOOT);
    return;
  }

  bool connected = s.phase == BLE_LINK_CONNECTED && s.seq > 0 && s.age_ms < 4000;

  if(!connected && was_connected)
  {
    buzz_play(BUZZ_LOST);
    screen_wake(SCREEN_LOST_MS);
  }

  was_connected = connected;

  // ---- screen on / dark ----
  bool keep = now - started_ms < BOOT_MS + 10000;               // the first 10 s

  if(imu_present() && imu_looking())
    keep = true;                                                 // you are looking

  if(imu_teach_state() == 1)
    keep = true;                                                 // teaching your looking angle

  if(connected)
  {
    if(s.strike && s.strikeid != strike_quiet)
      keep = true;                                               // strike: until answered

    if(!s.strike && (s.bucket >= PULL_SHOW || s.drop >= PULL_SHOW || s.dropwatch || strcmp(s.state, "pulltest") == 0))
      keep = true;                                               // something building / pull test
  }

  if(keep)
  {
    // on now; with the motion sensor it goes off at once when this ends
    screen_wake(imu_present() ? 0 : 1000);

    if((int32_t)(screen_until - now) < 150)
      screen_until = now + 150;
  }

  if(screen_on && (int32_t)(now - screen_until) > 0)
    screen_dark();

  if(!connected)
  {
    layout(MODE_LOOKING);

    spin = (spin + 12) % 360;
    lv_arc_set_rotation(radar_arc, spin);       // the radar sweeps round

    set_text(title, "Looking for the rod");
    set_text_color(title, COL_TEXT);
    // what the link is doing, so a problem can be seen without a computer
    char info[96];

    if(s.phase == BLE_LINK_SEARCHING && s.scans > 0)
      snprintf(info, sizeof(info), "scan %d - %d devices seen\n%s", s.scans, s.seen,
               s.note[0] ? s.note : "rod not seen yet");
    else
      snprintf(info, sizeof(info), "%s", s.note[0] ? s.note : "Bluetooth - stay within ~30 m");

    set_text(subtitle, info);
    set_text_color(subtitle, COL_DIM);
    return;
  }

  top_icons(&s);

  if(s.strike || ((page != 0 || vpage != 0) && now - page_since > PAGE_BACK_MS))
  {
    page = 0;
    vpage = 0;
  }

  page_dots();

  if(vpage == -1)
  {
    battery_page(&s);
    return;
  }

  if(vpage == 1)
  {
    last_strike_page(&s, now);
    return;
  }

  if(page == 1)
  {
    sea_page(&s);
    return;
  }

  if(page == -1)
  {
    relearn_page(&s, now);

    if(page == -1)
      return;
  }

  // a new knock: show it for 3 s
  bool new_knock = last_knock_id >= 0 && s.knockid != last_knock_id;

  if(new_knock)
  {
    knock_until = now + 3000;

    if(!imu_present())
      screen_wake(SCREEN_KNOCK_MS);   // with the motion sensor a knock only vibrates
  }

  last_knock_id = s.knockid;

  // ---- vibration ----
  strike_seen = s.strikeid;

  if(s.strike)
  {
    // a vibration for each state, until you answer; reminder every 30 s
    if(s.strikeid != strike_quiet)
    {
      if(buzz_strike_at == 0)
      {
        buzz_play(BUZZ_STRIKE);           // the strike itself
        buzz_strike_at = now;
        buzz_state_at = now;
        buzz_remind_at = now;
      }
      else if(fish != fish_was)
      {
        buzz_play(fish == FISH_RUN ? BUZZ_STRIKE : fish == FISH_KNOCK ? BUZZ_KNOCK :
                  fish == FISH_SLACK ? BUZZ_DROP : BUZZ_TAP);
        buzz_state_at = now;

        if(fish != FISH_QUIET)
          buzz_remind_at = now;
      }
      else if(fish == FISH_RUN && now - buzz_state_at >= 1500)
      {
        buzz_play(BUZZ_STRIKE);           // still running: long buzz every 1.5 s
        buzz_state_at = now;
        buzz_remind_at = now;
      }
      else if(fish == FISH_SLACK && now - buzz_state_at >= 3000)
      {
        buzz_play(BUZZ_DROP);
        buzz_state_at = now;
        buzz_remind_at = now;
      }
      else if(fish == FISH_KNOCK && new_knock)
      {
        buzz_play(BUZZ_KNOCK);
        buzz_remind_at = now;
      }
      else if(now - buzz_remind_at >= 30000)
      {
        buzz_play(BUZZ_STRIKE);           // reminder: not answered yet
        buzz_remind_at = now;
      }
    }

    fish_was = fish;
  }
  else
  {
    buzz_strike_at = 0;
    fish_was = fish;
    bool drop = s.dropwatch || s.drop >= PULL_SHOW;

    if(drop && !drop_was)
      buzz_play(BUZZ_DROP);
    else if(new_knock)
      buzz_play(BUZZ_KNOCK);
    else if(s.bucket >= PULL_SHOW && now - buzz_pull_at >= (uint32_t)(s.bucket >= 90 ? 500 : 2000 - s.bucket * 15))
    {
      buzz_pull_at = now;
      buzz_play(BUZZ_PULL);
    }

    drop_was = drop;
  }

  char a[48];
  char b[48];

  if(s.strike)
  {
    strike_page(&s, now);
    return;
  }

  if(strcmp(s.state, "paused") == 0)
  {
    layout(MODE_REELED);
    ring_set(0, COL_CYAN);

    pill_set("REELED IN", COL_DIM, false);
    set_text(title, "Cast,");
    set_text_color(title, COL_TEXT);
    set_text(subtitle, "then tap READY");
    set_text_color(subtitle, COL_DIM);

    set_button(btn1, btn1_label, &btn1_path, s.command_busy ? "..." : LV_SYMBOL_PLAY, "/ready", 268, 112, 112, COL_CYAN, lv_color_hex(0x04121a));
    set_text(btn1_cap, "READY");
    return;
  }

  if(strcmp(s.state, "pulltest") == 0)
  {
    layout(MODE_PULLTEST);

    float frac = s.ptneed > 0 ? s.ptdeg / s.ptneed : 0;
    frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);

    ring_set((int)(frac * 100), COL_ORANGE);
    pill_set("PULL TEST", COL_ORANGE, true);

    snprintf(a, sizeof(a), "%.1f", s.ptdeg < 0 ? 0 : s.ptdeg);
    set_text(title, a);
    set_text_color(title, COL_TEXT);
    snprintf(a, sizeof(a), "of %.1f deg", s.ptneed);
    set_text(subtitle, a);
    set_text_color(subtitle, COL_DIM);

    set_text(hint, frac >= 1 ? "good - now let go" : "pull the line gently, then let go");
    set_text_color(hint, frac >= 1 ? COL_ORANGE : COL_DIM);

    // the rod tip bends down as you pull
    int bend = (int)(frac * 22);

    if(pt_rod_pts[1].y != 18 + bend)
    {
      pt_rod_pts[1].y = 18 + bend;
      pt_line_pts[0].y = 18 + bend;
      lv_line_set_points(pt_rod, pt_rod_pts, 2);
      lv_line_set_points(pt_line, pt_line_pts, 2);
    }

    set_button(btn1, btn1_label, &btn1_path, s.command_busy ? "..." : LV_SYMBOL_NEXT, "/skippull", 352, 56, 56, COL_CARD, COL_TEXT);
    set_text(btn1_cap, "SKIP");
    return;
  }

  // ---------- home ----------
  layout(MODE_LIVE);

  lv_color_t c = COL_CYAN;
  bool warn = false;              // orange: something building
  int ringv = s.bucket;
  const char *pill = "ARMED";

  if(strcmp(s.state, "learning") == 0)
  {
    pill = "LEARNING";
    set_text(subtitle, "keep the rod still");
    c = COL_ORANGE;
    warn = true;
  }
  else if(strcmp(s.state, "settling") == 0)
  {
    pill = "SETTLING";
    set_text(subtitle, "learning the new position");
    c = COL_ORANGE;
    warn = true;
  }
  else if(s.dropwatch)
  {
    pill = "DROP-BACK";
    set_text(subtitle, "watching for a pull...");
    c = COL_ORANGE;
    warn = true;
    ringv = 100;
  }
  else if(s.bucket >= PULL_SHOW)
  {
    snprintf(b, sizeof(b), "SLOW PULL  %d%%", s.bucket);
    pill = b;
    set_text(subtitle, "building up " LV_SYMBOL_UP);
    c = COL_ORANGE;
    warn = true;
  }
  else if(s.drop >= PULL_SHOW)
  {
    snprintf(b, sizeof(b), "DROP-BACK  %d%%", s.drop);
    pill = b;
    set_text(subtitle, "line going slack");
    c = COL_ORANGE;
    warn = true;
    ringv = s.drop;
  }
  else if(now < knock_until)
  {
    pill = "KNOCK";
    snprintf(a, sizeof(a), "%d knock%s in the last 4 s", s.knocks, s.knocks == 1 ? "" : "s");
    set_text(subtitle, a);
  }
  else
  {
    snprintf(a, sizeof(a), "waves %s  -  wind %s", wave_name(s.wave), wind_name(s.wind));
    set_text(subtitle, s.dir ? a : "no pull test: knocks only");
  }

  pill_set(pill, c, true);
  set_text_color(subtitle, warn ? COL_ORANGE : COL_DIM);
  ring_set(ringv < 4 ? 4 : ringv, c);

  // TILT left, KNOCKS (or PULL while it builds) right
  snprintf(a, sizeof(a), "%+.1f", s.tilt);
  metric(0, a, "deg", "TILT", warn ? COL_ORANGE : COL_VIOLET, 104);

  if(s.bucket >= PULL_SHOW)
  {
    snprintf(a, sizeof(a), "%d", s.bucket);
    metric(1, a, "%", "PULL", COL_ORANGE, 104);
  }
  else
  {
    snprintf(a, sizeof(a), "%d", s.knocks);
    metric(1, a, "", "KNOCKS", s.knocks > 0 ? COL_CYAN : COL_TEXT, 104);
  }

  graph_now();

  bool reel = strcmp(s.state, "learning") != 0;
  show(st_btn[1], reel);
  show(st_btn_cap[1], reel);
  set_text(st_btn_cap[1], s.command_busy ? "..." : "REEL IN");
}

// ---------- every 100 ms: graph (200 points = 20 s) ----------

static void graph_timer(lv_timer_t *t)
{
  (void)t;

  ble_link_status_t s;
  ble_link_get(&s, true);

  if(s.phase != BLE_LINK_CONNECTED || s.seq == 0)
    return;

  int k = s.knockthr > 0 ? (int)(s.energy_peak / s.knockthr * KNOCK_LIMIT) : 0;
  int tl = (int)(s.tilt * 100);

  lv_chart_set_next_value(chart, ser_knock, k > 300 ? 300 : k);
  lv_chart_set_next_value(chart, ser_limit, KNOCK_LIMIT);
  lv_chart_set_next_value(chart, ser_tilt, tl > 300 ? 300 : (tl < -300 ? -300 : tl));

  fish_track(&s, k);

  // the graph panel (home + STRIKE) shows the same data (shared arrays, same start point)
  if(mode == MODE_STRIKE || mode == MODE_LIVE)
  {
    uint16_t sp = lv_chart_get_x_start_point(chart, ser_knock);
    lv_chart_set_x_start_point(st_bars, st_bar_ser, sp);
    lv_chart_set_x_start_point(st_lines, st_knock_ser, sp);
    lv_chart_set_x_start_point(st_lines, st_tilt_ser, sp);
    lv_chart_refresh(st_bars);
    lv_chart_refresh(st_lines);
  }
}

// ---------- building the screen ----------

void watch_ui_create(void)
{
  scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // soft glow behind the ring
  ring_glow = gauge_arc(scr, 452, 24, COL_BG);
  lv_obj_center(ring_glow);
  lv_obj_set_style_arc_opa(ring_glow, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(ring_glow, LV_OPA_10, LV_PART_INDICATOR);

  // outer gauge ring: 270 degrees, open at the bottom
  ring = gauge_arc(scr, 452, 10, COL_RING_BG);
  lv_obj_center(ring);

  // top: Bluetooth signal bars, rod battery
  for(int i = 0; i < 4; i++)
  {
    bars[i] = box(scr, 4, 5 + i * 4, 1, COL_OFF);
    lv_obj_align(bars[i], LV_ALIGN_TOP_MID, -32 + i * 6, 60 - (5 + i * 4));
  }

  bat_label = label(scr, FONT_MICRO, COL_DIM);
  lv_obj_align(bat_label, LV_ALIGN_TOP_MID, 14, 44);

  // title + subtitle
  title = label(scr, FONT_BIG, COL_CYAN);
  subtitle = label(scr, FONT_SMALL, COL_DIM);

  // looking for the rod: radar rings, Bluetooth dot, a sweeping arc
  for(int i = 0; i < 3; i++)
  {
    int r = 60 + i * 30;
    radar[i] = box(scr, 2 * r, 2 * r, LV_RADIUS_CIRCLE, COL_BG);
    lv_obj_set_style_bg_opa(radar[i], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(radar[i], 2, 0);
    lv_obj_set_style_border_color(radar[i], COL_BLUE, 0);
    lv_obj_set_style_border_opa(radar[i], i == 0 ? LV_OPA_50 : i == 1 ? LV_OPA_30 : LV_OPA_20, 0);
    lv_obj_align(radar[i], LV_ALIGN_CENTER, 0, -33);
  }

  radar_dot = box(scr, 58, 58, LV_RADIUS_CIRCLE, COL_BLUE);
  lv_obj_align(radar_dot, LV_ALIGN_CENTER, 0, -33);
  lv_obj_t *bt = label(radar_dot, FONT_MID, lv_color_white());
  lv_label_set_text(bt, LV_SYMBOL_BLUETOOTH);
  lv_obj_center(bt);

  radar_arc = lv_arc_create(scr);
  lv_obj_set_size(radar_arc, 240, 240);
  lv_obj_align(radar_arc, LV_ALIGN_CENTER, 0, -33);
  lv_arc_set_bg_angles(radar_arc, 0, 360);
  lv_arc_set_range(radar_arc, 0, 100);
  lv_arc_set_value(radar_arc, 22);
  lv_obj_remove_style(radar_arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(radar_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_opa(radar_arc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(radar_arc, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(radar_arc, COL_CYAN, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(radar_arc, true, LV_PART_INDICATOR);

  // live data holder (not shown: the graph panel below draws it)
  chart = lv_chart_create(scr);
  lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, GRAPH_POINTS);
  lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -300, 300);     // knock 0..300 = top half
  lv_chart_set_range(chart, LV_CHART_AXIS_SECONDARY_Y, -300, 900);   // tilt -3..+3 deg = bottom half

  ser_limit = lv_chart_add_series(chart, lv_color_hex(0x6b5426), LV_CHART_AXIS_PRIMARY_Y);
  ser_tilt = lv_chart_add_series(chart, COL_VIOLET, LV_CHART_AXIS_SECONDARY_Y);
  ser_knock = lv_chart_add_series(chart, COL_CYAN, LV_CHART_AXIS_PRIMARY_Y);

  lv_chart_set_all_value(chart, ser_limit, KNOCK_LIMIT);
  lv_chart_set_all_value(chart, ser_tilt, 0);
  lv_chart_set_all_value(chart, ser_knock, 0);

  // pull test: a rod that bends, and the line going into the sea
  pt_rod = lv_line_create(scr);
  lv_line_set_points(pt_rod, pt_rod_pts, 2);
  lv_obj_set_pos(pt_rod, 0, 196);
  lv_obj_set_style_line_width(pt_rod, 5, 0);
  lv_obj_set_style_line_color(pt_rod, lv_color_hex(0x5f6b80), 0);
  lv_obj_set_style_line_rounded(pt_rod, true, 0);

  pt_line = lv_line_create(scr);
  lv_line_set_points(pt_line, pt_line_pts, 2);
  lv_obj_set_pos(pt_line, 0, 196);
  lv_obj_set_style_line_width(pt_line, 2, 0);
  lv_obj_set_style_line_color(pt_line, COL_CYAN, 0);

  // sea state page: two small gauges, an icon, the level, a caption
  static lv_point_t wave_pts[7] = { { 0, 6 }, { 6, 0 }, { 12, 6 }, { 18, 0 }, { 24, 6 }, { 30, 0 }, { 36, 6 } };
  static lv_point_t wind_pts[3][2] = { { { 0, 0 }, { 30, 0 } }, { { 0, 0 }, { 22, 0 } }, { { 0, 0 }, { 30, 0 } } };
  const char *sea_caps[2] = { "WAVES", "WIND" };

  for(int r = 0; r < 2; r++)
  {
    int cx = r == 0 ? -83 : 83;
    lv_color_t c = r == 0 ? COL_CYAN : COL_VIOLET;

    sea_arc[r] = gauge_arc(scr, 124, 8, COL_RING_BG);
    lv_obj_align(sea_arc[r], LV_ALIGN_TOP_MID, cx, 174);

    for(int i = 0; i < 3; i++)
    {
      sea_icon[r][i] = lv_line_create(scr);
      lv_obj_set_style_line_width(sea_icon[r][i], 3, 0);
      lv_obj_set_style_line_color(sea_icon[r][i], c, 0);
      lv_obj_set_style_line_rounded(sea_icon[r][i], true, 0);

      if(r == 0)
      {
        lv_line_set_points(sea_icon[r][i], wave_pts, 7);
        lv_obj_align(sea_icon[r][i], LV_ALIGN_TOP_MID, cx, 204 + i * 9);
      }
      else
      {
        lv_line_set_points(sea_icon[r][i], wind_pts[i], 2);
        lv_obj_align(sea_icon[r][i], LV_ALIGN_TOP_LEFT, 233 + cx - 15, 206 + i * 8);
      }
    }

    sea_val[r] = label(scr, FONT_SMALL, COL_TEXT);
    lv_obj_align(sea_val[r], LV_ALIGN_TOP_MID, cx, 236);

    sea_cap[r] = label(scr, FONT_MICRO, COL_DIM);
    lv_label_set_text(sea_cap[r], sea_caps[r]);
    lv_obj_align(sea_cap[r], LV_ALIGN_TOP_MID, cx, 294);
  }

  // small dim text (pull test help, sea state footer)
  hint = label(scr, FONT_MICRO, COL_DIM);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 330);

  // setup page: three list cells, icon - name - small text - arrow
  const char *cell_names[3] = { "Relearn the sea", "New pull direction", "Teach my look" };
  const char *cell_icons[3] = { LV_SYMBOL_REFRESH, LV_SYMBOL_UP, LV_SYMBOL_EYE_OPEN };
  const lv_color_t cell_cols[3] = { COL_ORANGE, COL_ORANGE, COL_CYAN };

  for(int i = 0; i < 3; i++)
  {
    cell[i] = lv_btn_create(scr);
    lv_obj_set_size(cell[i], 312, 68);
    lv_obj_align(cell[i], LV_ALIGN_TOP_MID, 0, 112 + i * 80);
    lv_obj_set_style_radius(cell[i], 18, 0);
    lv_obj_set_style_bg_color(cell[i], COL_CARD, 0);
    lv_obj_set_style_border_width(cell[i], 1, 0);
    lv_obj_set_style_border_color(cell[i], COL_LINE, 0);
    lv_obj_set_style_shadow_width(cell[i], 0, 0);
    lv_obj_set_style_pad_all(cell[i], 0, 0);
    lv_obj_set_style_clip_corner(cell[i], true, 0);
    lv_obj_clear_flag(cell[i], LV_OBJ_FLAG_SCROLLABLE);

    cell_fill[i] = box(cell[i], 1, 68, 0, COL_ORANGE);
    lv_obj_set_style_bg_opa(cell_fill[i], LV_OPA_30, 0);
    lv_obj_add_flag(cell_fill[i], LV_OBJ_FLAG_HIDDEN);

    // the icon in a tinted circle: one label with its own background
    cell_ico[i] = label(cell[i], FONT_SMALL, cell_cols[i]);
    lv_label_set_text(cell_ico[i], cell_icons[i]);
    lv_obj_set_size(cell_ico[i], 40, 40);
    lv_obj_set_style_radius(cell_ico[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(cell_ico[i], LV_OPA_20, 0);
    lv_obj_set_style_bg_color(cell_ico[i], cell_cols[i], 0);
    lv_obj_set_style_pad_top(cell_ico[i], 10, 0);
    lv_obj_align(cell_ico[i], LV_ALIGN_LEFT_MID, 14, 0);

    cell_name[i] = label(cell[i], FONT_SMALL, COL_TEXT);
    lv_label_set_text(cell_name[i], cell_names[i]);
    lv_obj_align(cell_name[i], LV_ALIGN_TOP_LEFT, 68, 12);

    cell_sub[i] = label(cell[i], FONT_MICRO, COL_DARK);
    lv_obj_align(cell_sub[i], LV_ALIGN_TOP_LEFT, 68, 38);

    cell_arrow[i] = label(cell[i], FONT_TINY, COL_DARK);
    lv_label_set_text(cell_arrow[i], LV_SYMBOL_RIGHT);
    lv_obj_align(cell_arrow[i], LV_ALIGN_RIGHT_MID, -16, 0);

    lv_obj_add_flag(cell[i], LV_OBJ_FLAG_HIDDEN);
  }

  lv_obj_add_event_cb(cell[2], button_clicked, LV_EVENT_CLICKED, (void *)"#teach");

  // battery page: two rings (outer = rod, inner = watch), time left in the middle
  const char *bat_names[2] = { "ROD", "WATCH" };
  const char *left_names[2] = { "rod left", "watch left" };

  for(int r = 0; r < 2; r++)
  {
    int cx = r == 0 ? -80 : 80;

    bat_arc[r] = gauge_arc(scr, r == 0 ? 240 : 190, 14, COL_RING_BG);
    lv_obj_align(bat_arc[r], LV_ALIGN_CENTER, 0, -37);

    bat_left[r] = label(scr, r == 0 ? FONT_MID : FONT_SMALL, COL_TEXT);
    lv_obj_align(bat_left[r], LV_ALIGN_TOP_MID, 0, r == 0 ? 146 : 202);

    bat_leftcap[r] = label(scr, FONT_MICRO, COL_DIM);
    lv_label_set_text(bat_leftcap[r], left_names[r]);
    lv_obj_align(bat_leftcap[r], LV_ALIGN_TOP_MID, 0, r == 0 ? 180 : 228);

    bat_pct[r] = label(scr, FONT_SMALL, COL_TEXT);
    lv_obj_align(bat_pct[r], LV_ALIGN_TOP_MID, cx + 8, 330);

    bat_dot[r] = box(scr, 10, 10, LV_RADIUS_CIRCLE, COL_DIM);
    lv_obj_align(bat_dot[r], LV_ALIGN_TOP_MID, cx - 30, 337);

    bat_cap[r] = label(scr, FONT_MICRO, COL_DIM);
    lv_label_set_text(bat_cap[r], bat_names[r]);
    lv_obj_align(bat_cap[r], LV_ALIGN_TOP_MID, cx + 8, 358);
  }

  // last strike page: graph of the rod's recording, in the same dark panel
  ls_chart = lv_chart_create(scr);
  lv_obj_set_size(ls_chart, 354, 130);
  lv_obj_align(ls_chart, LV_ALIGN_TOP_MID, 0, 124);
  lv_chart_set_type(ls_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(ls_chart, LS_MAX);
  lv_chart_set_div_line_count(ls_chart, 0, 0);
  lv_chart_set_range(ls_chart, LV_CHART_AXIS_PRIMARY_Y, -300, 300);
  lv_chart_set_range(ls_chart, LV_CHART_AXIS_SECONDARY_Y, -300, 900);
  lv_obj_set_style_bg_color(ls_chart, lv_color_hex(0x0c101a), 0);
  lv_obj_set_style_border_width(ls_chart, 1, 0);
  lv_obj_set_style_border_color(ls_chart, lv_color_hex(0x1e2432), 0);
  lv_obj_set_style_radius(ls_chart, 18, 0);
  lv_obj_set_style_pad_all(ls_chart, 10, 0);
  lv_obj_set_style_size(ls_chart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(ls_chart, 3, LV_PART_ITEMS);
  lv_obj_add_flag(ls_chart, LV_OBJ_FLAG_CLICKABLE);              // tap: ZOOM <-> 30 s
  lv_obj_clear_flag(ls_chart, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(ls_chart, ls_chart_clicked, LV_EVENT_CLICKED, NULL);

  ls_limit = lv_chart_add_series(ls_chart, lv_color_hex(0x6b5426), LV_CHART_AXIS_PRIMARY_Y);
  ls_tilt = lv_chart_add_series(ls_chart, COL_VIOLET, LV_CHART_AXIS_SECONDARY_Y);
  ls_knock = lv_chart_add_series(ls_chart, COL_CYAN, LV_CHART_AXIS_PRIMARY_Y);

  // red strike line with a dot on top: inside the graph (can never stick out of it)
  ls_mark = box(ls_chart, 2, 130 - 20, 0, COL_RED);
  lv_obj_add_flag(ls_mark, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_set_style_bg_opa(ls_mark, LV_OPA_70, 0);

  ls_mark_dot = box(ls_chart, 10, 10, LV_RADIUS_CIRCLE, COL_RED);
  lv_obj_add_flag(ls_mark_dot, LV_OBJ_FLAG_IGNORE_LAYOUT);

  // its story bar: what the fish did, left to right
  ls_story = story_canvas(ls_story_buf, ls_story_drawn, 262);

  ls_info[0] = label(scr, FONT_MICRO, COL_DIM);          // ZOOM 2 s | 2 s - tap
  lv_obj_align(ls_info[0], LV_ALIGN_TOP_MID, 0, 274);
  ls_info[1] = label(scr, FONT_TINY, COL_DIM);           // no strike yet / errors
  lv_obj_align(ls_info[1], LV_ALIGN_TOP_MID, 0, 180);
  ls_info[2] = label(scr, FONT_MICRO, COL_DIM);          // 3 strikes since ...
  lv_obj_align(ls_info[2], LV_ALIGN_TOP_MID, 0, 364);

  // page dots at the bottom
  for(int i = 0; i < 3; i++)
  {
    dots[i] = box(scr, 8, 8, LV_RADIUS_CIRCLE, COL_OFF);
    lv_obj_align(dots[i], LV_ALIGN_TOP_MID, -14 + i * 14, 440);
  }

  // big round button (READY / SKIP) and its name under it
  btn1 = button(&btn1_label);
  btn1_cap = label(scr, FONT_TINY, COL_DIM);

  // ---------- STRIKE screen ----------
  st_time = label(scr, FONT_TINY, COL_DIM);
  lv_obj_align(st_time, LV_ALIGN_TOP_MID, 0, 42);

  st_pill = lv_obj_create(scr);
  lv_obj_remove_style_all(st_pill);
  lv_obj_set_size(st_pill, LV_SIZE_CONTENT, 30);
  lv_obj_set_style_radius(st_pill, 15, 0);
  lv_obj_set_style_bg_opa(st_pill, LV_OPA_20, 0);
  lv_obj_set_style_border_width(st_pill, 1, 0);
  lv_obj_set_style_border_opa(st_pill, LV_OPA_60, 0);
  lv_obj_set_style_pad_hor(st_pill, 12, 0);
  lv_obj_set_flex_flow(st_pill, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(st_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(st_pill, 7, 0);
  lv_obj_clear_flag(st_pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(st_pill, LV_ALIGN_TOP_MID, -10, 66);

  st_dot = lv_obj_create(st_pill);
  lv_obj_remove_style_all(st_dot);
  lv_obj_set_size(st_dot, 8, 8);
  lv_obj_set_style_radius(st_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(st_dot, LV_OPA_COVER, 0);

  st_state = label(st_pill, FONT_TINY, COL_RED);

  st_trend = label(scr, FONT_SMALL, COL_ORANGE);

  const char *caps[2] = { "PULL", "TILT" };
  const char *units[2] = { "%", "deg" };

  for(int i = 0; i < 2; i++)
  {
    int cx = i == 0 ? -73 : 73;

    st_val[i] = label(scr, FONT_BIG, i == 0 ? COL_TEXT : COL_VIOLET);
    lv_obj_align(st_val[i], LV_ALIGN_TOP_MID, cx - 10, 104);

    st_unit[i] = label(scr, FONT_TINY, i == 0 ? COL_TEXT : COL_VIOLET);
    lv_label_set_text(st_unit[i], units[i]);
    lv_obj_align(st_unit[i], LV_ALIGN_TOP_MID, cx + 38, 120);

    st_cap[i] = label(scr, FONT_TINY, COL_DIM);
    lv_label_set_text(st_cap[i], caps[i]);
    lv_obj_align(st_cap[i], LV_ALIGN_TOP_MID, cx, 144);
  }

  // graph panel: knock as a filled area (bars), tilt as a line
  st_panel = lv_obj_create(scr);
  lv_obj_remove_style_all(st_panel);
  lv_obj_set_size(st_panel, 354, 126);
  lv_obj_align(st_panel, LV_ALIGN_TOP_MID, 0, 172);
  lv_obj_set_style_radius(st_panel, 18, 0);
  lv_obj_set_style_bg_opa(st_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(st_panel, lv_color_hex(0x0c101a), 0);
  lv_obj_set_style_border_width(st_panel, 1, 0);
  lv_obj_set_style_border_color(st_panel, lv_color_hex(0x1e2432), 0);
  lv_obj_set_style_clip_corner(st_panel, true, 0);
  lv_obj_clear_flag(st_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  // knock area: bars in the top half (0..300 = knock limit at 100)
  st_bars = lv_chart_create(st_panel);
  lv_obj_remove_style_all(st_bars);
  lv_obj_set_size(st_bars, 354, 10 + 53);
  lv_obj_set_pos(st_bars, 0, 0);
  lv_obj_set_style_pad_top(st_bars, 10, 0);
  lv_obj_set_style_pad_hor(st_bars, 10, 0);
  lv_chart_set_type(st_bars, LV_CHART_TYPE_BAR);
  lv_chart_set_point_count(st_bars, GRAPH_POINTS);
  lv_chart_set_div_line_count(st_bars, 0, 0);
  lv_chart_set_range(st_bars, LV_CHART_AXIS_PRIMARY_Y, 0, 300);
  lv_obj_set_style_pad_column(st_bars, 0, 0);
  lv_obj_set_style_radius(st_bars, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_grad_color(st_bars, lv_color_hex(0x0b3346), LV_PART_ITEMS);
  lv_obj_set_style_bg_grad_dir(st_bars, LV_GRAD_DIR_VER, LV_PART_ITEMS);
  lv_obj_clear_flag(st_bars, LV_OBJ_FLAG_CLICKABLE);
  st_bar_ser = lv_chart_add_series(st_bars, COL_CYAN, LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_ext_y_array(st_bars, st_bar_ser, lv_chart_get_y_array(chart, ser_knock));

  // knock edge + tilt line, same scales as the main graph
  st_lines = lv_chart_create(st_panel);
  lv_obj_remove_style_all(st_lines);
  lv_obj_set_size(st_lines, 354, 126);
  lv_obj_set_pos(st_lines, 0, 0);
  lv_obj_set_style_pad_all(st_lines, 10, 0);
  lv_chart_set_type(st_lines, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(st_lines, GRAPH_POINTS);
  lv_chart_set_div_line_count(st_lines, 0, 0);
  lv_chart_set_range(st_lines, LV_CHART_AXIS_PRIMARY_Y, -300, 300);
  lv_chart_set_range(st_lines, LV_CHART_AXIS_SECONDARY_Y, -300, 900);
  lv_obj_set_style_size(st_lines, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(st_lines, 3, LV_PART_ITEMS);
  lv_obj_clear_flag(st_lines, LV_OBJ_FLAG_CLICKABLE);
  st_knock_ser = lv_chart_add_series(st_lines, COL_CYAN, LV_CHART_AXIS_PRIMARY_Y);
  st_tilt_ser = lv_chart_add_series(st_lines, COL_VIOLET, LV_CHART_AXIS_SECONDARY_Y);
  lv_chart_set_ext_y_array(st_lines, st_knock_ser, lv_chart_get_y_array(chart, ser_knock));
  lv_chart_set_ext_y_array(st_lines, st_tilt_ser, lv_chart_get_y_array(chart, ser_tilt));

  // knock limit: dashed amber line
  static lv_point_t limit_pts[2] = { { 10, 0 }, { 344, 0 } };
  lv_obj_t *limit = lv_line_create(st_panel);
  lv_line_set_points(limit, limit_pts, 2);
  lv_obj_set_y(limit, 10 + (300 - KNOCK_LIMIT) * 106 / 600);
  lv_obj_set_style_line_color(limit, COL_ORANGE, 0);
  lv_obj_set_style_line_opa(limit, LV_OPA_50, 0);
  lv_obj_set_style_line_width(limit, 1, 0);
  lv_obj_set_style_line_dash_width(limit, 5, 0);
  lv_obj_set_style_line_dash_gap(limit, 5, 0);

  // strike marker: dashed red line with a dot on top
  static lv_point_t mark_pts[2] = { { 1, 0 }, { 1, 106 } };
  st_mark = lv_line_create(st_panel);
  lv_line_set_points(st_mark, mark_pts, 2);
  lv_obj_set_y(st_mark, 12);
  lv_obj_set_style_line_color(st_mark, COL_RED, 0);
  lv_obj_set_style_line_width(st_mark, 2, 0);
  lv_obj_set_style_line_dash_width(st_mark, 5, 0);
  lv_obj_set_style_line_dash_gap(st_mark, 4, 0);

  st_mark_dot = lv_obj_create(st_panel);
  lv_obj_remove_style_all(st_mark_dot);
  lv_obj_set_size(st_mark_dot, 10, 10);
  lv_obj_set_y(st_mark_dot, 4);
  lv_obj_set_style_radius(st_mark_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(st_mark_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(st_mark_dot, COL_RED, 0);

  // "now": dot at the end of the tilt line
  st_now = lv_obj_create(st_panel);
  lv_obj_remove_style_all(st_now);
  lv_obj_set_size(st_now, 12, 12);
  lv_obj_set_x(st_now, 354 - 10 - 6);
  lv_obj_set_style_radius(st_now, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(st_now, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(st_now, lv_color_white(), 0);
  lv_obj_set_style_border_width(st_now, 3, 0);
  lv_obj_set_style_border_color(st_now, COL_VIOLET, 0);
  lv_obj_set_style_shadow_width(st_now, 12, 0);
  lv_obj_set_style_shadow_color(st_now, COL_VIOLET, 0);
  lv_obj_set_style_shadow_opa(st_now, LV_OPA_60, 0);

  // story bar: last 30 s, one cell per second
  st_story = story_canvas(st_story_buf, st_story_drawn, 306);

  for(int i = 0; i < STORY_CELLS; i++)
    story[i] = FISH_QUIET;

  // round icon buttons: I'M WATCHING, REEL IN
  static const char *st_paths[2] = { "/watching", "/reelin" };
  const char *icons[2] = { LV_SYMBOL_EYE_OPEN, LV_SYMBOL_REFRESH };
  const char *bcaps[2] = { "WATCHING", "REEL IN" };

  for(int i = 0; i < 2; i++)
  {
    lv_color_t bc = i == 0 ? COL_CYAN : COL_RED;

    st_btn[i] = lv_btn_create(scr);
    lv_obj_set_size(st_btn[i], 64, 64);
    lv_obj_align(st_btn[i], LV_ALIGN_TOP_MID, i == 0 ? -57 : 57, 322);
    lv_obj_set_style_radius(st_btn[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(st_btn[i], lv_color_hex(0x141924), 0);
    lv_obj_set_style_border_width(st_btn[i], 2, 0);
    lv_obj_set_style_border_color(st_btn[i], bc, 0);
    lv_obj_set_style_shadow_width(st_btn[i], 0, 0);
    lv_obj_add_event_cb(st_btn[i], button_clicked, LV_EVENT_CLICKED, (void *)st_paths[i]);

    st_btn_icon[i] = label(st_btn[i], FONT_SMALL, bc);
    lv_label_set_text(st_btn_icon[i], icons[i]);
    lv_obj_center(st_btn_icon[i]);

    st_btn_cap[i] = label(scr, FONT_TINY, COL_DIM);
    lv_label_set_text(st_btn_cap[i], bcaps[i]);
    lv_obj_align(st_btn_cap[i], LV_ALIGN_TOP_MID, i == 0 ? -57 : 57, 392);
  }

  // boot logo on top of everything for the first seconds
  boot_img = lv_img_create(scr);
  lv_img_set_src(boot_img, &strikesense_boot_fixed);
  lv_obj_center(boot_img);

  started_ms = lv_tick_get();
  mode = MODE_LIVE;               // force the first layout
  layout(MODE_BOOT);

  // how full LVGL's memory is (Serial Monitor): if it is full the screen freezes
  lv_mem_monitor_t mm;
  lv_mem_monitor(&mm);
  printf("[UI] LVGL memory: %u of %u bytes used (%d%%)\n",
         (unsigned)(mm.total_size - mm.free_size), (unsigned)mm.total_size, mm.used_pct);

  lv_timer_create(ui_timer, 100, NULL);
  lv_timer_create(graph_timer, 100, NULL);
  lv_timer_create(swipe_timer, 20, NULL);
}

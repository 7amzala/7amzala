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
// The ring on the home and STRIKE screens shows live how hard the rod tip
// is pulled: it fills from the far-left end and moves with the tip (up,
// down, bouncing). Empty at rest and when the tip springs back. Full =
// strike level.
//
// STRIKE screen: what the fish does now - RUN (the rod is moving: bouncing,
// jerking, swinging more than the waves - or bent strongly and held there),
// KNOCK, SLACK (tip sprang back, line loose), QUIET (calm for 2 s: stopped,
// or swimming toward you) - with a trend arrow, PULL % and TILT, the live
// graph with the strike marked, and a 30 s story bar. Until WATCHING or
// REEL IN: RUN = fast short pulses (faster and stronger with a harder pull),
// on the strike screen the ring empties within ~1 s once the rod is still;
// KNOCK click per kick
// + 3 clicks / 5 s; SLACK buzz-click-buzz / 4 s; QUIET wake-up alarm, 3
// long strong buzzes / 5 s, after 30 s longer ones / 4 s.
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

// The screens need 64 KB of LVGL memory. build_opt.h (in this sketch folder)
// gives LVGL its settings, so no lv_conf.h is needed. 48 KB froze the watch.
#if defined(LV_MEM_SIZE) && LV_MEM_SIZE < (64U * 1024U) && !defined(WATCH_UI_HOST_TEST)
#error "build_opt.h is missing: keep it in the StrikeSense_Watch folder, next to StrikeSense_Watch.ino"
#endif
#include "ble_link.h"
#include "buzz.h"
#include "watch_bat.h"
#include "watch_imu.h"
#include "watch_power.h"
#include "watch_clock.h"
#include "strike_log.h"
#include "strike_sd.h"

void lcd_set_brightness(uint8_t level);   // lcd_bsp.c
#include "strikesense_boot_fixed.h"
#include <stdio.h>
#include <stdlib.h>
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
#define SWIPE_JUMP   160          // a finger can't move this far in one reading: a bad touch read
#define SWIPE_MIN_MS 80           // a swipe takes at least this long
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
// the live graph's data (last 20 s): plain arrays the graph panel draws
// from (no hidden chart object: less screen memory)
static lv_coord_t g_knock[GRAPH_POINTS];
static lv_coord_t g_tilt[GRAPH_POINTS];
static uint16_t g_start = 0;          // the oldest point
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
#define CELLS 5
static lv_obj_t *cell[CELLS];
static lv_obj_t *cell_fill[CELLS];     // fills up while held
static lv_obj_t *cell_ico[CELLS];
static lv_obj_t *cell_name[CELLS];
static lv_obj_t *cell_sub[CELLS];
static lv_obj_t *cell_arrow[CELLS];

// the link / LoRa channel choice just tapped (shown until the rod confirms)
static int want_link = -1, want_chm = -1;
static uint32_t want_at = 0;

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
static int ls_view = 0;               // 0 = close-up (6 s, drag) / rod: zoom; 1 = whole strike; tap to switch

// strike history (the watch's own recordings, strike_log.c)
#define HIST_W     334                // points drawn at most (one per pixel)
#define HIST_CLOSE 60                 // close-up: 6 s - a real zoom (a whole strike is often only 25-35 s)
static lv_coord_t ls_k[HIST_W], ls_t[HIST_W], ls_lim[HIST_W];
#define ls_ago    title               // "2 h 15 min ago" (the title is not used on that page)
static lv_obj_t *ls_home;             // round home button
static lv_obj_t *ls_axis[4];          // times under the graph
static lv_obj_t *ls_win;              // the part shown, on the story bar
#define home_last hint                // home screen: "last strike 18 min ago" (hint not used there)
static slog_t *hist_rec_shown = NULL;
static int hist_view_shown = -1, hist_off_shown = -1, hist_n_shown = -1;
static int hist_off = 0;              // close-up: first point shown
static slog_t *hist_rec_for_off = NULL;
static lv_coord_t ls_drag_x0;
static int ls_drag_off0;
static bool ls_moved = false;

// back to the live graph when something happens (strike: at once)
static bool pending_home = false;
static uint32_t last_knock_jump = 0;
static bool finger_down = false;
static bool bucket_was_on = false, drop_was_on = false;

static uint32_t last_settime_ms = 0;  // the watch told the rod the time
static uint32_t sd_save_at = 0;

static int hist_total(void);
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
static bool fish_mov_init = false;
static float fish_avg = 0;            // tip angle, 0.5 s average
static float fish_mov2 = 0;           // movement around it, squared
static float fish_mov = 0;            // movement (deg RMS)
static float fish_mov_base = 0.1f;    // the waves' normal movement
static float fish_moving_level = 0.25f; // movement above this = moving
static uint32_t fish_move_ms = 0;     // last time the rod moved
static float st_rest = 0;             // strike screen: the tip's rest (follows it once the rod is still)
#define FISH_MOVE_MIN 0.25f           // deg: at least this much movement = moving
#define FISH_CALM_MS  2000            // calm this long = not running
static float fish_slow = 0;           // strength, slow average (for the trend)
static int fish_trend = 0;            // +1 stronger, -1 weaker, 0 steady
static fish_t story[STORY_CELLS];
static int story_head = 0;
static fish_t story_cell = FISH_QUIET;
static uint32_t story_at = 0;
static uint32_t strike_ms = 0;        // when the strike on the screen started
static long strike_ms_id = -1;
static uint32_t buzz_state_at = 0;
static uint32_t quiet_from = 0;       // the fish went quiet (strike alarm gets stronger after 30 s)

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

// The strike graph: drag left / right to move through the strike
// (close-up), tap to switch close-up <-> whole strike
static void ls_chart_event(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *in = lv_indev_get_act();
  lv_point_t p = { 0, 0 };

  if(in)
    lv_indev_get_point(in, &p);

  if(code == LV_EVENT_PRESSED)
  {
    ls_drag_x0 = p.x;
    ls_drag_off0 = hist_off;
    ls_moved = false;
    return;
  }

  if(code == LV_EVENT_PRESSING)
  {
    int dx = p.x - ls_drag_x0;

    if(dx > 12 || dx < -12)
      ls_moved = true;                  // (a tap may wobble a few pixels)

    if(ls_moved && ls_view == 0)
      hist_off = ls_drag_off0 - dx * HIST_CLOSE / (354 - 20);   // finger right = earlier

    return;
  }

  if(code == LV_EVENT_RELEASED && !ls_moved && !swiped)
  {
    ls_view = 1 - ls_view;
    ls_shown_ms = 0;                  // draw again
    hist_view_shown = -1;
    buzz_play(BUZZ_TAP);
  }
}

static void home_clicked(lv_event_t *e)
{
  (void)e;

  if(swiped)
    return;

  page = 0;
  vpage = 0;
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

  // WATCH LINK / LoRa CHANNEL: each tap = the next choice, kept on the rod (the app shows it too)
  if(path != NULL && (strcmp(path, "#link") == 0 || strcmp(path, "#chan") == 0) && !swiped)
  {
    ble_link_status_t s;
    ble_link_get(&s, false);
    bool link = path[1] == 'l';
    static const char *links[3] = { "auto", "bt", "lora" };
    static const char *chans[4] = { "auto", "fast", "long1", "long2" };
    char cmd[32];

    if(link)
    {
      int now_m = want_link >= 0 && lv_tick_get() - want_at < 3000 ? want_link : s.lmode;
      want_link = ((now_m < 0 ? 0 : now_m) + 1) % 3;
      snprintf(cmd, sizeof(cmd), "/linkmode?m=%s", links[want_link]);
    }
    else
    {
      int now_c = want_chm >= 0 && lv_tick_get() - want_at < 3000 ? want_chm : s.chm;
      want_chm = ((now_c < 0 ? 0 : now_c) + 1) % 4;
      snprintf(cmd, sizeof(cmd), "/lorachan?m=%s", chans[want_chm]);
    }

    want_at = lv_tick_get();
    ble_link_command(cmd);
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

// ---- live ring (home, strike): how hard the rod tip is pulled, now ----
// It fills from the far-left end like a gauge as the tip is pulled toward
// the sea, and follows it live: up, down, bouncing with the rod. Empty at
// rest and when the tip springs back (the label says SLACK). Full = 3x the
// wave allowance (strike level). The rod sends the tip 10x a second; the
// ring slides there in 3 even steps (one per 33 ms frame), so it moves as
// fast or as slowly as the tip - nothing is added or smoothed away.
static bool ring_live = false;
static int ring_cur = 0, ring_from = 0, ring_to = 0, ring_step = 0;

// live vibration during a strike (RUN, not answered): follows the ring
static bool haptic_live = false;
static uint32_t haptic_kick_until = 0;          // a knock: one strong kick

#define RING_STEPS 3

static void ring_mode(bool live)
{
  if(live == ring_live)
    return;

  ring_live = live;
  ring_cur = ring_from = ring_to = 0;
  ring_step = RING_STEPS;
  lv_arc_set_value(ring, 0);
  lv_arc_set_value(ring_glow, 0);
}

static void ring_pull(float tip_deg, float allow, lv_color_t c)
{
  float full = 3 * (allow > 0.2f ? allow : 0.2f);
  int v = (int)(tip_deg / full * 100);

  v = v > 100 ? 100 : (v < 2 ? 0 : v);           // at rest or springing back: empty

  for(int i = 0; i < 2; i++)
  {
    lv_obj_t *a = i == 0 ? ring : ring_glow;

    if(lv_obj_get_style_arc_color(a, LV_PART_INDICATOR).full != c.full)
      lv_obj_set_style_arc_color(a, c, LV_PART_INDICATOR);
  }

  if(v != ring_to)
  {
    ring_from = ring_cur;
    ring_to = v;
    ring_step = 0;
  }
}

static void ring_timer(lv_timer_t *tm)
{
  (void)tm;

  if(ring_live && ring_step < RING_STEPS)
  {
    ring_step++;
    ring_cur = ring_from + (ring_to - ring_from) * ring_step / RING_STEPS;
    lv_arc_set_value(ring, ring_cur);
    lv_arc_set_value(ring_glow, ring_cur);
  }

}

// RUN vibration: short fast pulses, "bz-bz-bz" like a bee - about 6 a
// second for a gentle pull up to 14 for a hard one, and stronger too; a
// knock gives one extra-strong pulse.
static float haptic_phase = 0;
static float haptic_strength = 0;               // 0..1, set by the strike screen

static void haptic_timer(lv_timer_t *tm)
{
  (void)tm;

  if(!haptic_live)
  {
    haptic_phase = 0;
    buzz_live_stop();
    return;
  }

  float st = haptic_strength < 0 ? 0 : (haptic_strength > 1 ? 1 : haptic_strength);
  float rate = 6 + 8 * st;                      // pulses a second

  haptic_phase += rate * 0.010f;

  if(haptic_phase >= 1)
    haptic_phase -= 1;

  bool on = haptic_phase < 0.5f;                // half on, half off
  int level = on ? 150 + (int)(105 * st) : 0;

  if((int32_t)(haptic_kick_until - lv_tick_get()) > 0)
    level = 255;

  buzz_live(level);
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
  int last = (g_start + GRAPH_POINTS - 1) % GRAPH_POINTS;
  int tv = g_tilt[last];
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
  ring_mode(live || st);
  lv_obj_set_style_arc_width(ring, st || m == MODE_PULLTEST ? 12 : 10, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(ring, st || m == MODE_PULLTEST ? 12 : 10, LV_PART_MAIN);

  // top: signal + rod battery (the strike screen shows the time there)
  bool top = live || sea || m == MODE_REELED || m == MODE_PULLTEST || m == MODE_RELEARN;

  show(bat_label, top);

  for(int i = 0; i < 4; i++)
    show(bars[i], top);

  // page dots: only on the three side-by-side pages
  for(int i = 0; i < 3; i++)
    show(dots[i], live || sea || m == MODE_RELEARN);

  // state pill: everywhere but the radar, boot and battery
  show(st_pill, !look && !bat && m != MODE_BOOT);
  lv_obj_align(st_pill, LV_ALIGN_TOP_MID, st ? -10 : 0, lst ? 32 : 66);
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
  for(int i = 0; i < CELLS; i++)
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
  show(ls_home, lst);
  show(ls_win, false);

  for(int i = 0; i < 4; i++)
    show(ls_axis[i], false);

  for(int i = 0; i < 3; i++)
    show(ls_info[i], lst);

  // texts: which page uses the title / subtitle / hint
  show(title, look || m == MODE_REELED || m == MODE_PULLTEST || lst);   // strike page: "... ago" 
  show(subtitle, look || live || m == MODE_REELED || m == MODE_PULLTEST || lst);
  show(hint, m == MODE_PULLTEST || sea || live);                      // home: "last strike ... ago" 

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
      place(hint, FONT_MICRO, 22);
      set_text_color(hint, COL_DIM);
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
      place(subtitle, FONT_TINY, 68);
      place(title, FONT_MICRO, 90);
      set_text_color(title, COL_DIM);
      break;

    default:
      break;
  }
}

// ---------- every 100 ms: state, texts, colours ----------

static void top_icons(const ble_link_status_t *s)
{
  bool lora = s->via == LINK_LORA;
  int n = lora ? s->lora_bars :
          s->rssi == 0 ? 0 : s->rssi >= -60 ? 4 : s->rssi >= -70 ? 3 : s->rssi >= -80 ? 2 : 1;

  for(int i = 0; i < 4; i++)
    set_bg_color(bars[i], i < n ? (lora ? COL_ORANGE : COL_CYAN) : COL_OFF);

  char t[40];

  if(s->bat < 0)
    snprintf(t, sizeof(t), "--");
  else
    snprintf(t, sizeof(t), "%d%%", s->bat);

  // orange bars + "LoRa 500m Clear": rod far away, which channel, how busy
  if(lora)
  {
    size_t n = strlen(t);
    snprintf(t + n, sizeof(t) - n, " LoRa %s %s", s->lora_chan == 0 ? "500m" : "1km",
             s->lora_quality == 0 ? "Clear" : s->lora_quality == 1 ? "Good" : s->lora_quality == 2 ? "Busy" : "Crowd");
  }

  set_text(bat_label, t);
  set_text_color(bat_label, s->bat >= 0 && s->bat < 15 ? COL_RED : lora ? COL_ORANGE : COL_DIM);
}

// ---------- swipe: watch the finger (also over buttons) ----------

static void swipe_timer(lv_timer_t *t)
{
  (void)t;

  static bool down = false;
  static lv_point_t from;
  static lv_point_t last;
  static uint32_t down_ms = 0;
  static int beyond = 0;              // readings in a row past the swipe distance

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
    last = p;
    down_ms = lv_tick_get();
    beyond = 0;
    swiped = !screen_on;            // dark: this touch only wakes the screen
    screen_wake(imu_present() ? SCREEN_TOUCH_MS : SCREEN_IDLE_MS);
    return;
  }

  finger_down = pressed;

  if(!pressed)
  {
    down = false;
    return;
  }

  // a jump no finger can make: a bad touch read, ignore this point
  int jx = p.x - last.x;
  int jy = p.y - last.y;

  if((jx < 0 ? -jx : jx) + (jy < 0 ? -jy : jy) > SWIPE_JUMP)
    return;

  last = p;

  int dx = p.x - from.x;
  int dy = p.y - from.y;
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;

  if(adx < SWIPE_PX && ady < SWIPE_PX)
  {
    beyond = 0;
    return;
  }

  // a real swipe: far enough on 2 readings in a row, and not instant
  if(swiped || ++beyond < 2 || lv_tick_get() - down_ms < SWIPE_MIN_MS)
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
    // finger up: battery, or back toward the live graph through the strikes
    if(vpage == 0)
      vpage = -1;
    else if(vpage >= 1)
      vpage--;
  }
  else
  {
    // finger down: newest strike, then older ones; or back from the battery
    if(vpage == 0)
      vpage = 1;
    else if(vpage == -1)
      vpage = 0;
    else if(vpage >= 1 && vpage < hist_total())
      vpage++;
  }

  if(vpage >= 1 && vpage != vbefore)
  {
    hist_rec_for_off = NULL;          // close-up, at the strike
    hist_view_shown = -1;

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

  int w = (int)(held * 290);
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

  show(cell[3], !teach && s->lmode >= 0);   // the rod tells its choices (rod 2.3+)
  show(cell[4], !teach && s->chm >= 0);     // (a rod with LoRa)

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

  // the choice just tapped until the rod has it, then what the rod says
  bool fresh = now - want_at < 3000;
  int lm = fresh && want_link >= 0 ? want_link : s->lmode;
  int cm = fresh && want_chm >= 0 ? want_chm : s->chm;
  static const char *link_names[3] = { "Auto  (tap to change)", "Bluetooth only", "LoRa only" };
  static const char *chan_names[4] = { "Auto: 500 m, then 1 km", "500 m fast", "1 km - A", "1 km - B" };

  cell_set(3, lm >= 0 && lm <= 2 ? link_names[lm] : "-", 0, false);
  cell_set(4, cm >= 0 && cm <= 3 ? chan_names[cm] : "-", 0, false);
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

  // How much the rod MOVES: the tip angle minus its 0.35 s average (slow
  // tide / bending removed), as RMS over ~0.5 s. Waves move it too, but
  // smoothly: their normal level is learned while nothing happens, and
  // the fish must move it clearly more (bouncing, jerking, swinging).
  if(!fish_mov_init)
  {
    fish_avg = s->ftilt;
    fish_mov_init = true;
  }

  fish_avg += (s->ftilt - fish_avg) * 0.25f;            // ~0.35 s at 10 a second
  float hp = s->ftilt - fish_avg;
  fish_mov2 += (hp * hp - fish_mov2) * 0.2f;            // ~0.5 s
  fish_mov = sqrtf(fish_mov2);

  if(!s->strike && s->bucket < PULL_SHOW && s->drop < PULL_SHOW && k < KNOCK_LIMIT)
    fish_mov_base += (fish_mov - fish_mov_base) * 0.004f; // ~25 s: the waves' normal level

  float moving_level = 2.0f * fish_mov_base;

  if(moving_level < FISH_MOVE_MIN)
    moving_level = FISH_MOVE_MIN;

  // RUN: the rod is moving (still pulling), or bent strongly forward and
  // held there (a steady run makes no bounce). The rod's pull % / drop-back
  // stay latched during a strike, so they are not used here.
  if(fish_mov > moving_level)
    fish_move_ms = now;                         // the rod is moving

  fish_moving_level = moving_level;

  if(fish_mov > moving_level || s->ftilt > 2 * allow)
    fish_run_ms = now;

  if(s->ftilt < -allow)
    fish_slack_ms = now;                        // tip sprang back: line went loose

  if(k >= KNOCK_LIMIT)
    fish_knock_ms = now;

  fish_t f = FISH_QUIET;

  if(fish_slack_ms && now - fish_slack_ms < 1000)
    f = FISH_SLACK;
  else if(fish_run_ms && now - fish_run_ms < FISH_CALM_MS)
    f = FISH_RUN;                               // calm for 2 s = not running any more
  else if(fish_knock_ms && now - fish_knock_ms < 3000)
    f = FISH_KNOCK;

  if(f != FISH_QUIET)
    fish_quiet_since = 0;
  else if(fish_quiet_since == 0)
    fish_quiet_since = now;

  fish = f;

  // strength 0..1 (for the trend arrow): how far the tip is out, knock size
  float st = fabsf(s->ftilt) / (allow * 3);
  float kk = k / 300.0f;

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
    st_rest = 0;
  }

  // The rod stopped moving and is not bent hard (a steady run): where it
  // is now is its rest - roughly back is back - so the ring empties within
  // about a second instead of creeping down with the last bit of bend.
  float allow = s->allow > 0.2f ? s->allow : 0.2f;

  if(now - fish_move_ms > 400 && s->ftilt <= 2 * allow)
    st_rest += (s->ftilt - st_rest) * 0.5f;

  float pulled = s->ftilt - st_rest;
  float ring_part = pulled / (3 * allow);
  float mov_part = fish_mov / (2 * fish_moving_level);

  haptic_strength = ring_part > mov_part ? ring_part : mov_part;

  lv_color_t c = fish_color(fish);

  // the ring: how hard the tip is pulled now, from its rest (red: a strike)
  ring_pull(pulled, s->allow, COL_RED);

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
  // PULL: how hard the tip is pulled now (the rod's pull % stays at 100 during a strike)
  int pull = ring_to > 0 ? ring_to : 0;
  snprintf(a, sizeof(a), "%d", pull);
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

// ---------- strike history (the watch's own recordings) ----------

static int hist_total(void)
{
  int n = slog_count();
  return n > 0 ? n : 1;               // none yet: the rod's last strike
}

static void fmt_ago(char *out, size_t size, long sec)
{
  if(sec < 0)
    out[0] = 0;
  else if(sec < 60)
    snprintf(out, size, "%ld s ago", sec);
  else if(sec < 3600)
    snprintf(out, size, "%ld min ago", sec / 60);
  else if(sec < 48 * 3600L)
    snprintf(out, size, "%ld h %02ld min ago", sec / 3600, (sec % 3600) / 60);
  else
    snprintf(out, size, "%ld days ago", sec / 86400);
}

// seconds since the strike: from the clock, or the watch's own count
static long rec_age(const slog_t *r, uint32_t now_ms)
{
  uint32_t c = clock_now();

  if(r->utc && c >= r->utc)
    return (long)(c - r->utc);

  if(!r->from_card && r->got_ms)
    return (long)((now_ms - r->got_ms) / 1000);

  return -1;
}

// "21:42" today, "14 Oct 21:42" another day, "" if the time was not known
static void fmt_when(char *out, size_t size, const slog_t *r)
{
  static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

  if(r->utc == 0)
  {
    out[0] = 0;
    return;
  }

  long local = (long)r->utc + r->tz * 60L;
  long day = local / 86400, sec = local % 86400;
  long today = clock_now() ? ((long)clock_now() + clock_tz() * 60L) / 86400 : day;

  if(day == today)
  {
    snprintf(out, size, "%02ld:%02ld", sec / 3600, (sec / 60) % 60);
    return;
  }

  // days since 1970 -> date
  long z = day + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
  long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);

  snprintf(out, size, "%d %s %02ld:%02ld", d, mon[m - 1], sec / 3600, (sec / 60) % 60);
}

static void fmt_secs(char *out, size_t size, long sec)
{
  if(sec < 0)
    snprintf(out, size, "-%ld:%02ld", -sec / 60, -sec % 60);
  else
    snprintf(out, size, "%ld:%02ld", sec / 60, sec % 60);
}

// the colour bar: the whole recording, one pixel column each
static void hist_story(const slog_t *r)
{
  for(int x = 0; x < STORY_W; x++)
  {
    int b0 = x * r->n / STORY_W, b1 = (x + 1) * r->n / STORY_W;
    int f = 0;

    for(int i = b0; i < b1 || i == b0; i++)
      if(i < r->n && r->st[i] > f)
        f = r->st[i];

    lv_color_t c = f == FISH_QUIET ? lv_color_hex(0x343a4a) : fish_color((fish_t)f);

    for(int y = 0; y < STORY_H; y++)
      ls_story_buf[y * STORY_W + x] = c;
  }

  memset(ls_story_drawn, -1, sizeof(ls_story_drawn));   // (the rod's page paints cells)
  lv_obj_invalidate(ls_story);
}

static void history_page(slog_t *r, int idx, int total, uint32_t now)
{
  layout(MODE_LASTSTRIKE);

  char a[72], w[24];

  snprintf(a, sizeof(a), "STRIKE %d OF %d", total - idx, total);
  pill_set(a, COL_RED, true);

  fmt_when(w, sizeof(w), r);
  snprintf(a, sizeof(a), "%s%s%s", r->type[0] ? r->type : "STRIKE", w[0] ? "  -  " : "", w);
  set_text(subtitle, a);
  set_text_color(subtitle, COL_TEXT);

  fmt_ago(a, sizeof(a), rec_age(r, now));

  if(!r->done)
    strncat(a, a[0] ? "  -  recording..." : "recording...", sizeof(a) - strlen(a) - 1);

  set_text(ls_ago, a);

  show(ls_chart, true);
  show(ls_story, true);
  set_text(ls_info[1], "");

  // which part: close-up (20 s, dragged) or the whole strike
  int n = r->n, a0 = 0, cnt = n;

  if(hist_rec_for_off != r)
  {
    hist_rec_for_off = r;
    hist_off = r->pre - 20;           // starts 2 s before the strike
  }

  bool close = ls_view == 0 && n > HIST_CLOSE;

  if(close)
  {
    hist_off = hist_off < 0 ? 0 : (hist_off > n - HIST_CLOSE ? n - HIST_CLOSE : hist_off);
    a0 = hist_off;
    cnt = HIST_CLOSE;
  }

  show(ls_win, close);

  for(int i = 0; i < 4; i++)
    show(ls_axis[i], cnt > 1);

  if(r != hist_rec_shown || ls_view != hist_view_shown || a0 != hist_off_shown || n != hist_n_shown)
  {
    bool new_rec = r != hist_rec_shown || n != hist_n_shown;

    hist_rec_shown = r;
    hist_view_shown = ls_view;
    hist_off_shown = a0;
    hist_n_shown = n;
    ls_shown_ms = 0;                  // (the rod's page must draw again)

    // the points: one per pixel at most (biggest knock, furthest tilt of each)
    int pts = cnt < HIST_W ? cnt : HIST_W;

    if(pts < 2)
      pts = 2;

    for(int i = 0; i < pts; i++)
    {
      int b0 = a0 + i * cnt / pts, b1 = a0 + (i + 1) * cnt / pts;
      int kk = 0, tt = 0;

      for(int j = b0; j < b1 || j == b0; j++)
      {
        if(j >= n)
          break;

        if(r->k[j] > kk) kk = r->k[j];
        if(abs(r->t[j]) >= abs(tt)) tt = r->t[j];
      }

      ls_k[i] = kk;
      ls_t[i] = tt;
      ls_lim[i] = KNOCK_LIMIT;
    }

    lv_chart_set_point_count(ls_chart, pts);
    lv_chart_refresh(ls_chart);

    // red line + dot at the strike, if it is in view
    bool in = r->pre >= a0 && r->pre < a0 + cnt;
    show(ls_mark, in);
    show(ls_mark_dot, in);

    if(in)
    {
      int x = (r->pre - a0) * (354 - 20) / (cnt > 1 ? cnt - 1 : 1);
      lv_obj_set_pos(ls_mark, x - 1, 0);
      lv_obj_set_pos(ls_mark_dot, x - 5, -8);
    }

    // times under the graph (0:00 = the strike)
    for(int i = 0; i < 4; i++)
    {
      long d = (long)(a0 + i * (cnt - 1) / 3) - r->pre;          // points (10 a second)
      long sec = d >= 0 ? (d + 5) / 10 : -((-d + 5) / 10);       // rounded
      char lab[16];

      if(sec == 0)
        snprintf(lab, sizeof(lab), "0:00");
      else if(sec > -60 && sec < 60)
        snprintf(lab, sizeof(lab), "%+lds", sec);
      else
        snprintf(lab, sizeof(lab), "%s%ld:%02ld", sec < 0 ? "-" : "+", labs(sec) / 60, labs(sec) % 60);

      set_text(ls_axis[i], lab);
      set_text_color(ls_axis[i], sec == 0 ? COL_RED : COL_DARK);
      lv_obj_align(ls_axis[i], LV_ALIGN_TOP_MID, -167 + i * 334 / 3, 252);
    }

    if(new_rec)
      hist_story(r);

    // the part shown, as a box on the colour bar
    if(close)
    {
      int x0 = a0 * STORY_W / n, x1 = (a0 + cnt) * STORY_W / n;
      lv_obj_set_size(ls_win, x1 - x0 + 4, STORY_H + 8);
      lv_obj_align(ls_win, LV_ALIGN_TOP_MID, -STORY_W / 2 + (x0 + x1) / 2, 270 - 4);
    }

    // the story in words + numbers
    int run = 0, slack = -1, last = r->pre, peak = 0;

    for(int i = r->pre; i < n; i++)
    {
      if(r->st[i] == FISH_RUN) run++;
      if(r->st[i] == FISH_SLACK && slack < 0) slack = i;
      if(r->st[i] != FISH_QUIET) last = i;
      if(r->t[i] > peak) peak = r->t[i];
    }

    char t1[16], t2[16];
    int len = snprintf(a, sizeof(a), "RUN %d s", run / 10);

    if(slack >= 0)
    {
      fmt_secs(t1, sizeof(t1), (slack - r->pre) / 10);
      len += snprintf(a + len, sizeof(a) - len, "  -  SLACK %s", t1);
    }

    if(r->done)
    {
      fmt_secs(t2, sizeof(t2), (last - r->pre) / 10);
      snprintf(a + len, sizeof(a) - len, "  -  quiet from %s", t2);
    }

    set_text(ls_info[0], a);

    snprintf(t1, sizeof(t1), "%+.1f", peak / 100.0f);
    metric(0, t1, "deg", "TILT PEAK", COL_VIOLET, 296);
    fmt_secs(t2, sizeof(t2), (last - r->pre) / 10);
    metric(1, t2, "", "LASTED", COL_TEXT, 296);
  }

  set_text(ls_info[2], close ? "drag < >  -  tap: whole" : "tap: close-up");
}

// none recorded by the watch yet: the rod's last strike (fetched)
static void rod_last_strike_page(const ble_link_status_t *s, uint32_t now)
{
  layout(MODE_LASTSTRIKE);

  show(ls_win, false);

  for(int i = 0; i < 4; i++)
    show(ls_axis[i], false);

  set_text(ls_ago, "");

  if(hist_rec_shown != NULL)
  {
    hist_rec_shown = NULL;            // the history page draws again next time
    ls_shown_ms = 0;
  }

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
    haptic_live = false;              // no rod: no live vibration
    layout(MODE_LOOKING);

    spin = (spin + 12) % 360;
    lv_arc_set_rotation(radar_arc, spin);       // the radar sweeps round

    set_text(title, "Looking for the rod");
    set_text_color(title, COL_TEXT);
    // what the link is doing, so a problem can be seen without a computer
    char info[128];

    if(s.phase == BLE_LINK_SEARCHING && s.scans > 0)
      snprintf(info, sizeof(info), "scan %d - %d devices seen\n%s", s.scans, s.seen,
               s.note[0] ? s.note : "rod not seen yet");
    else
      snprintf(info, sizeof(info), "%s", s.note[0] ? s.note : "Bluetooth - stay within ~30 m");

    // the LoRa backup, so a wiring problem shows without a computer
    size_t used = strlen(info);
    snprintf(info + used, sizeof(info) - used, "\n%s",
             s.lora_ok ? "LoRa ready - rod not heard" : "LoRa module not found");

    set_text(subtitle, info);
    set_text_color(subtitle, COL_DIM);
    return;
  }

  top_icons(&s);

  // ---- time and date: from the rod (set by the phone), or the watch tells a restarted rod ----
  if(s.time > 1600000000L)
  {
    uint32_t c = clock_now();
    long diff = c ? (long)(c - (uint32_t)s.time) : 9999;

    if(diff > 2 || diff < -2 || s.tz != clock_tz())
      clock_set((uint32_t)s.time, s.tz);
  }
  else if(s.time == 0 && clock_now() && now - last_settime_ms > 20000)
  {
    char path[48];
    snprintf(path, sizeof(path), "/settime?t=%lu&tz=%d", (unsigned long)clock_now(), clock_tz());
    ble_link_command(path);
    last_settime_ms = now;
  }

  // ---- finished strikes to the SD card, never during a strike ----
  if(!s.strike && !slog_recording() && now - sd_save_at > 1000)
  {
    sd_save_at = now;
    strike_sd_save();
  }

  // ---- something happened: back to the live graph ----
  // a new knock: show it for 3 s
  bool new_knock = last_knock_id >= 0 && s.knockid != last_knock_id;

  if(new_knock)
  {
    knock_until = now + 3000;

    if(!imu_present())
      screen_wake(SCREEN_KNOCK_MS);   // with the motion sensor a knock only vibrates
  }

  last_knock_id = s.knockid;

  bool bucket_on = s.bucket >= PULL_SHOW;
  bool drop_on = s.drop >= PULL_SHOW || s.dropwatch;
  bool away = page != 0 || vpage != 0;

  if(away && ((bucket_on && !bucket_was_on) || (drop_on && !drop_was_on) ||
              (new_knock && now - last_knock_jump >= 30000)))
  {
    pending_home = true;

    if(new_knock)
      last_knock_jump = now;
  }

  bucket_was_on = bucket_on;
  drop_was_on = drop_on;

  // not in the middle of something: holding a setup button, teaching
  // the look, a finger on the screen (then as soon as it lifts)
  if(pending_home && hold_btn == NULL && imu_teach_state() != 1 && !finger_down)
  {
    pending_home = false;
    page = 0;
    vpage = 0;
  }

  if(!away)
    pending_home = false;

  // a strike: at once; another page for 30 s: back by itself
  if(s.strike || (away && now - page_since > PAGE_BACK_MS))
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

  if(vpage >= 1)
  {
    if(slog_count() > 0)
    {
      int i = vpage - 1 < slog_count() ? vpage - 1 : slog_count() - 1;
      history_page(slog_get(i), i, slog_count(), now);
    }
    else
    {
      rod_last_strike_page(&s, now);
    }

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

  // ---- vibration ----
  strike_seen = s.strikeid;

  if(s.strike)
  {
    // until you answer:
    //   RUN   live: the wrist feels the rod (stronger the harder it pulls,
    //         pulsing as it bounces, a kick on each knock)
    //   KNOCK a click per kick + click click click every 5 s
    //   SLACK buzz . click . buzz every 4 s
    //   QUIET wake-up alarm: 3 long strong buzzes every 5 s, after 30 s
    //         longer ones every 4 s
    bool answered = s.strikeid == strike_quiet;

    haptic_live = !answered && fish == FISH_RUN && buzz_strike_at != 0 && now - buzz_strike_at > 1200;

    if(!answered)
    {
      if(fish == FISH_QUIET && fish_was != FISH_QUIET)
        quiet_from = now;

      bool late = fish == FISH_QUIET && now - quiet_from >= 30000;
      static const buzz_t pattern[] = { BUZZ_ALARM, BUZZ_KNOCKS, BUZZ_SLACK, BUZZ_STRIKE };   // by fish_t
      uint32_t every = fish == FISH_QUIET ? (late ? 4000 : 5000) : fish == FISH_KNOCK ? 5000 : 4000;

      if(buzz_strike_at == 0)
      {
        buzz_play(BUZZ_STRIKE);           // the strike itself
        buzz_strike_at = now;
        buzz_state_at = now;
        quiet_from = now;
      }
      else if(fish == FISH_RUN)
      {
        if(new_knock)
          haptic_kick_until = now + 80;   // (live vibration does the rest)
      }
      else if(fish != fish_was || now - buzz_state_at >= every)
      {
        buzz_play(late ? BUZZ_ALARM2 : pattern[fish]);   // changed, or again: not answered yet
        buzz_state_at = now;
      }
      else if(fish == FISH_KNOCK && new_knock)
      {
        buzz_play(BUZZ_KNOCK);            // each kick
      }
    }

    fish_was = fish;
  }
  else
  {
    buzz_strike_at = 0;
    haptic_live = false;
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

  // top: how long ago the last strike was
  {
    char ago[40] = "", line[56] = "";

    if(slog_count() > 0)
      fmt_ago(ago, sizeof(ago), rec_age(slog_get(0), now));

    if(ago[0])
      snprintf(line, sizeof(line), "last strike %s", ago);

    set_text(home_last, line);
  }

  lv_color_t c = COL_CYAN;
  bool warn = false;              // orange: something building
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
  ring_pull(s.ftilt, s.allow, c);

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

  g_knock[g_start] = k > 300 ? 300 : k;
  g_tilt[g_start] = tl > 300 ? 300 : (tl < -300 ? -300 : tl);
  g_start = (g_start + 1) % GRAPH_POINTS;

  fish_track(&s, k);

  // the watch records each strike itself (strike_log.c)
  slog_tick(k, tl, fish, s.strike, s.strikeid, s.type, s.allow, s.strikeid == strike_quiet,
            lv_tick_get(), clock_now(), clock_tz());

  // the graph panel (home + STRIKE) shows the same data (shared arrays, same start point)
  if(mode == MODE_STRIKE || mode == MODE_LIVE)
  {
    uint16_t sp = g_start;
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
  lv_obj_align(bat_label, LV_ALIGN_TOP_LEFT, 233, 44);     // grows to the right ("85% LoRa")


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

  // setup page: five list cells, icon - name - small text - arrow
  const char *cell_names[CELLS] = { "Relearn the sea", "New pull direction", "Teach my look", "Watch link", "LoRa channel" };
  const char *cell_icons[CELLS] = { LV_SYMBOL_REFRESH, LV_SYMBOL_UP, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_BLUETOOTH, LV_SYMBOL_WIFI };
  const lv_color_t cell_cols[CELLS] = { COL_ORANGE, COL_ORANGE, COL_CYAN, COL_CYAN, COL_CYAN };

  for(int i = 0; i < CELLS; i++)
  {
    cell[i] = lv_btn_create(scr);
    lv_obj_set_size(cell[i], 290, 56);
    lv_obj_align(cell[i], LV_ALIGN_TOP_MID, 0, 96 + i * 64);
    lv_obj_set_style_radius(cell[i], 18, 0);
    lv_obj_set_style_bg_color(cell[i], COL_CARD, 0);
    lv_obj_set_style_border_width(cell[i], 1, 0);
    lv_obj_set_style_border_color(cell[i], COL_LINE, 0);
    lv_obj_set_style_shadow_width(cell[i], 0, 0);
    lv_obj_set_style_pad_all(cell[i], 0, 0);
    lv_obj_set_style_clip_corner(cell[i], true, 0);
    lv_obj_clear_flag(cell[i], LV_OBJ_FLAG_SCROLLABLE);

    cell_fill[i] = box(cell[i], 1, 56, 0, COL_ORANGE);
    lv_obj_set_style_bg_opa(cell_fill[i], LV_OPA_30, 0);
    lv_obj_add_flag(cell_fill[i], LV_OBJ_FLAG_HIDDEN);

    // the icon in a tinted circle: one label with its own background
    cell_ico[i] = label(cell[i], FONT_SMALL, cell_cols[i]);
    lv_label_set_text(cell_ico[i], cell_icons[i]);
    lv_obj_set_size(cell_ico[i], 36, 36);
    lv_obj_set_style_radius(cell_ico[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(cell_ico[i], LV_OPA_20, 0);
    lv_obj_set_style_bg_color(cell_ico[i], cell_cols[i], 0);
    lv_obj_set_style_pad_top(cell_ico[i], 8, 0);
    lv_obj_align(cell_ico[i], LV_ALIGN_LEFT_MID, 12, 0);

    cell_name[i] = label(cell[i], FONT_SMALL, COL_TEXT);
    lv_label_set_text(cell_name[i], cell_names[i]);
    lv_obj_align(cell_name[i], LV_ALIGN_TOP_LEFT, 60, 6);

    cell_sub[i] = label(cell[i], FONT_MICRO, COL_DARK);
    lv_obj_align(cell_sub[i], LV_ALIGN_TOP_LEFT, 60, 31);

    cell_arrow[i] = label(cell[i], FONT_TINY, COL_DARK);
    lv_label_set_text(cell_arrow[i], LV_SYMBOL_RIGHT);
    lv_obj_align(cell_arrow[i], LV_ALIGN_RIGHT_MID, -16, 0);

    lv_obj_add_flag(cell[i], LV_OBJ_FLAG_HIDDEN);
  }

  lv_obj_add_event_cb(cell[2], button_clicked, LV_EVENT_CLICKED, (void *)"#teach");
  lv_obj_add_event_cb(cell[3], button_clicked, LV_EVENT_CLICKED, (void *)"#link");
  lv_obj_add_event_cb(cell[4], button_clicked, LV_EVENT_CLICKED, (void *)"#chan");

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
  lv_obj_set_size(ls_chart, 354, 136);
  lv_obj_align(ls_chart, LV_ALIGN_TOP_MID, 0, 112);
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
  lv_obj_add_flag(ls_chart, LV_OBJ_FLAG_CLICKABLE);              // drag: move in time, tap: close-up <-> whole
  lv_obj_clear_flag(ls_chart, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(ls_chart, ls_chart_event, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(ls_chart, ls_chart_event, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(ls_chart, ls_chart_event, LV_EVENT_RELEASED, NULL);

  ls_limit = lv_chart_add_series(ls_chart, lv_color_hex(0x6b5426), LV_CHART_AXIS_PRIMARY_Y);
  ls_tilt = lv_chart_add_series(ls_chart, COL_VIOLET, LV_CHART_AXIS_SECONDARY_Y);
  ls_knock = lv_chart_add_series(ls_chart, COL_CYAN, LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_ext_y_array(ls_chart, ls_limit, ls_lim);     // our arrays: no screen memory used
  lv_chart_set_ext_y_array(ls_chart, ls_tilt, ls_t);
  lv_chart_set_ext_y_array(ls_chart, ls_knock, ls_k);

  // red strike line with a dot on top: inside the graph (can never stick out of it)
  ls_mark = box(ls_chart, 2, 136 - 20, 0, COL_RED);
  lv_obj_add_flag(ls_mark, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_set_style_bg_opa(ls_mark, LV_OPA_70, 0);

  ls_mark_dot = box(ls_chart, 10, 10, LV_RADIUS_CIRCLE, COL_RED);
  lv_obj_add_flag(ls_mark_dot, LV_OBJ_FLAG_IGNORE_LAYOUT);

  // its story bar: what the fish did, left to right
  ls_story = story_canvas(ls_story_buf, ls_story_drawn, 270);

  // the part shown in the close-up: a box on the colour bar
  ls_win = box(scr, 40, STORY_H + 8, 4, COL_BG);
  lv_obj_set_style_bg_opa(ls_win, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ls_win, 2, 0);
  lv_obj_set_style_border_color(ls_win, COL_TEXT, 0);

  for(int i = 0; i < 4; i++)
    ls_axis[i] = label(scr, FONT_MICRO, COL_DARK);

  // round home button: straight back to the live graph
  ls_home = lv_btn_create(scr);
  lv_obj_set_size(ls_home, 52, 52);
  lv_obj_align(ls_home, LV_ALIGN_TOP_MID, 0, 362);
  lv_obj_set_style_radius(ls_home, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ls_home, COL_CARD, 0);
  lv_obj_set_style_border_width(ls_home, 2, 0);
  lv_obj_set_style_border_color(ls_home, COL_CYAN, 0);
  lv_obj_set_style_shadow_width(ls_home, 0, 0);
  lv_obj_add_event_cb(ls_home, home_clicked, LV_EVENT_CLICKED, NULL);
  lv_obj_t *hi = label(ls_home, FONT_SMALL, COL_CYAN);
  lv_label_set_text(hi, LV_SYMBOL_HOME);
  lv_obj_center(hi);

  ls_info[0] = label(scr, FONT_MICRO, COL_TEXT);         // RUN 14 s - SLACK 0:21 - ... / rod: ZOOM 2 s | 2 s
  lv_obj_align(ls_info[0], LV_ALIGN_TOP_MID, 0, 280);
  ls_info[1] = label(scr, FONT_TINY, COL_DIM);           // no strike yet / errors
  lv_obj_align(ls_info[1], LV_ALIGN_TOP_MID, 0, 180);
  ls_info[2] = label(scr, FONT_MICRO, COL_DIM);          // drag < > - tap ... / rod: 3 strikes since ...
  lv_obj_align(ls_info[2], LV_ALIGN_TOP_MID, 0, 422);

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
  lv_chart_set_ext_y_array(st_bars, st_bar_ser, g_knock);

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
  lv_chart_set_ext_y_array(st_lines, st_knock_ser, g_knock);
  lv_chart_set_ext_y_array(st_lines, st_tilt_ser, g_tilt);

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
  lv_timer_create(ring_timer, 33, NULL);
  lv_timer_create(haptic_timer, 10, NULL);
}

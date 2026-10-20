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
// Wrist vibration (buzz.c, if the Haptic Bzzz is wired): knock = tick,
// slow pull = pulses (faster as it grows), drop-back = 2 ticks, strike =
// strong buzz every 1.5 s until a button is pressed, rod lost = 3 buzzes.
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
static lv_obj_t *bt_icon;
static lv_obj_t *bars[4];
static lv_obj_t *bat_label;
static lv_obj_t *title;
static lv_obj_t *subtitle;
static lv_obj_t *chart;
static lv_chart_series_t *ser_knock;
static lv_chart_series_t *ser_limit;
static lv_chart_series_t *ser_tilt;
static lv_obj_t *chart_knock_label;
static lv_obj_t *chart_tilt_label;
static lv_obj_t *cap1;
static lv_obj_t *val1;
static lv_obj_t *cap2;
static lv_obj_t *val2;
static lv_obj_t *btn1;
static lv_obj_t *btn1_label;
static lv_obj_t *btn2;
static lv_obj_t *btn2_label;
static lv_obj_t *btn3;                // only on the RELEARN page
static lv_obj_t *btn3_label;
static lv_obj_t *bar;
static lv_obj_t *dots[3];
static lv_obj_t *sea_cap[2];
static lv_obj_t *sea_val[2];
static lv_obj_t *sea_bar[2][4];
static lv_obj_t *hint;

// pages: left/right: -1 relearn, 0 main, 1 sea state
//        up/down:    -1 battery, 0 (the left/right page), 1 last strike
static int page = 0;
static int vpage = 0;

// battery page
static lv_obj_t *bat_cap[2];
static lv_obj_t *bat_pct[2];
static lv_obj_t *bat_bar[2];
static lv_obj_t *bat_left[2];
static lv_obj_t *bat_leftcap[2];

// last strike page
static lv_obj_t *ls_chart;
static lv_chart_series_t *ls_knock;
static lv_chart_series_t *ls_limit;
static lv_chart_series_t *ls_tilt;
static lv_obj_t *ls_mark;             // red line: the strike moment
static lv_obj_t *ls_mark_label;
static lv_obj_t *ls_info[3];
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
static const char *btn2_path = "";
static const char *btn3_path = "";
static lv_obj_t *hold_btn = NULL;     // the hold-2-s button being held

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

static lv_obj_t *button(lv_obj_t **text, const char **path_slot)
{
  lv_obj_t *b = lv_btn_create(scr);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);

  *text = label(b, FONT_SMALL, COL_TEXT);
  lv_obj_center(*text);

  (void)path_slot;
  return b;
}

// Place a button, its text, colours and what it sends
static void set_button(lv_obj_t *b, lv_obj_t *t, const char **slot, const char *text, const char *path,
                       int y, int w, int h, lv_color_t bg, lv_color_t fg)
{
  show(b, true);

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
static bool spinning = false;

static void ring_set(int value, lv_color_t colour)
{
  if(spinning)
  {
    spinning = false;
    lv_arc_set_rotation(ring, 270);
  }

  lv_arc_set_value(ring, value < 0 ? 0 : (value > 100 ? 100 : value));   // no redraw if the same

  if(lv_obj_get_style_arc_color(ring, LV_PART_INDICATOR).full != colour.full)
    lv_obj_set_style_arc_color(ring, colour, LV_PART_INDICATOR);
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

static void layout(ui_mode_t m)
{
  if(m == mode)
    return;

  mode = m;

  bool live = m == MODE_LIVE;

  show(boot_img, m == MODE_BOOT);
  show(chart, live);
  show(chart_knock_label, live);
  show(chart_tilt_label, live);
  show(cap1, live);
  show(val1, live);
  show(cap2, live);
  show(val2, live);
  show(bar, m == MODE_PULLTEST);
  show(hint, m == MODE_RELEARN || m == MODE_BATTERY || m == MODE_LASTSTRIKE);

  for(int r = 0; r < 2; r++)
  {
    show(bat_cap[r], m == MODE_BATTERY);
    show(bat_pct[r], m == MODE_BATTERY);
    show(bat_bar[r], m == MODE_BATTERY);
    show(bat_left[r], m == MODE_BATTERY);
    show(bat_leftcap[r], m == MODE_BATTERY);
  }

  show(ls_chart, m == MODE_LASTSTRIKE);
  show(ls_mark, m == MODE_LASTSTRIKE);
  show(ls_mark_label, m == MODE_LASTSTRIKE);

  for(int i = 0; i < 3; i++)
    show(ls_info[i], m == MODE_LASTSTRIKE);

  for(int r = 0; r < 2; r++)
  {
    show(sea_cap[r], m == MODE_SEA);
    show(sea_val[r], m == MODE_SEA);

    for(int i = 0; i < 4; i++)
      show(sea_bar[r][i], m == MODE_SEA);
  }
  show(btn1, false);
  show(btn2, false);
  show(btn3, false);

  bool top = m != MODE_BOOT && m != MODE_LOOKING && m != MODE_BATTERY && m != MODE_LASTSTRIKE;

  show(bt_icon, top);
  show(bat_label, top);

  for(int i = 0; i < 4; i++)
    show(bars[i], top);

  for(int i = 0; i < 3; i++)
    show(dots[i], top && m != MODE_STRIKE && m != MODE_BATTERY && m != MODE_LASTSTRIKE);

  show(ring, m != MODE_BOOT);

  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_set_style_arc_width(ring, m == MODE_STRIKE || m == MODE_PULLTEST ? 14 : 10, LV_PART_INDICATOR);

  switch(m)
  {
    case MODE_LOOKING:
      lv_obj_set_style_text_font(title, FONT_MID, 0);
      lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 140);
      lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 300);
      break;

    case MODE_LIVE:
      lv_obj_set_style_text_font(title, FONT_BIG, 0);
      lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 88);
      lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 138);
      break;

    case MODE_STRIKE:
      lv_obj_set_style_text_font(title, FONT_HUGE, 0);
      lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 92);
      lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 165);
      break;

    case MODE_REELED:
      lv_obj_set_style_text_font(title, FONT_BIG, 0);
      lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 120);
      lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 180);
      break;

    case MODE_SEA:
      lv_obj_set_style_text_font(title, FONT_BIG, 0);
      lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 84);
      lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 128);
      break;

    case MODE_RELEARN:
      lv_obj_set_style_text_font(title, FONT_BIG, 0);
      lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 76);
      lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 120);
      lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 302);
      break;

    case MODE_BATTERY:
      lv_obj_set_style_text_font(title, FONT_BIG, 0);
      lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);
      lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 404);
      break;

    case MODE_LASTSTRIKE:
      lv_obj_set_style_text_font(title, FONT_BIG, 0);
      lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);
      lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 98);
      lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 412);
      break;

    case MODE_PULLTEST:
      lv_obj_set_size(bar, 260, 12);
      lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 262);
      lv_obj_set_style_text_font(title, FONT_BIG, 0);
      lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 95);
      lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 150);
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
    set_bg_color(bars[i], i < n ? COL_CYAN : lv_color_hex(0x262b36));

  char t[24];

  if(s->bat < 0)
    snprintf(t, sizeof(t), LV_SYMBOL_BATTERY_EMPTY " --");
  else
    snprintf(t, sizeof(t), "%s %d%%",
             s->bat > 80 ? LV_SYMBOL_BATTERY_FULL : s->bat > 55 ? LV_SYMBOL_BATTERY_3 :
             s->bat > 30 ? LV_SYMBOL_BATTERY_2 : s->bat > 12 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY,
             s->bat);

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
  set_text_color(sea_val[r], c);

  for(int i = 0; i < 4; i++)
    set_bg_color(sea_bar[r][i], i < level + 1 ? c : lv_color_hex(0x262b36));
}

static void sea_page(const ble_link_status_t *s)
{
  layout(MODE_SEA);
  ring_set(0, COL_BLUE);

  set_text(title, "SEA STATE");
  set_text_color(title, COL_TEXT);
  set_text(subtitle, "from the rod's sensor");
  set_text_color(subtitle, COL_DIM);

  sea_row(0, wave_name(s->wave), s->wave, COL_CYAN);
  sea_row(1, wind_name(s->wind), s->wind, COL_VIOLET);
}

static uint32_t teach_shown = 0;       // when the teach result appeared

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

  if(ts == 1 || result)
  {
    show(btn1, false);
    show(btn2, false);
    show(btn3, false);
    set_text(title, ts == 1 ? "HOLD STILL" : ts == 2 ? "LEARNED" : "MOVED");
    set_text_color(title, ts == 3 ? COL_ORANGE : COL_CYAN);
    set_text(subtitle, ts == 1 ? "hold the watch the way\nyou look at it" : ts == 2 ? "your looking angle\nis saved" : "try again,\nhold it still");
    set_text_color(subtitle, COL_TEXT);
    set_text(hint, "");
    ring_set((int)(imu_teach_progress() * 100), ts == 3 ? COL_ORANGE : COL_CYAN);
    return;
  }

  set_text(title, "SETUP");
  set_text_color(title, COL_ORANGE);
  set_text(subtitle, "hold a button 2 seconds");
  set_text_color(subtitle, COL_DIM);
  set_text(hint, "");

  // RELEARN THE SEA and NEW PULL DIRECTION: hold 2 s (no accidents)
  lv_obj_t *held_now = NULL;

  if(!swiped)
  {
    if(lv_obj_has_state(btn1, LV_STATE_PRESSED))
      held_now = btn1;
    else if(lv_obj_has_state(btn2, LV_STATE_PRESSED))
      held_now = btn2;
  }

  if(held_now != hold_btn)
  {
    hold_btn = held_now;
    hold_from = held_now ? now : 0;
  }

  uint32_t held = hold_btn ? now - hold_from : 0;

  if(held >= HOLD_MS)
  {
    bool sea = hold_btn == btn1;
    ble_link_command(sea ? "/relearn" : "/teachdir");
    buzz_play(BUZZ_DROP);             // two ticks: done
    hold_btn = NULL;
    hold_from = 0;
    swiped = true;                    // this touch is used up: letting go presses nothing
    page = 0;                         // back to the main page: LEARNING or PULL TEST
    vpage = 0;
    return;
  }

  ring_set((int)(held * 100 / HOLD_MS), COL_ORANGE);

  bool h1 = hold_btn == btn1;
  bool h2 = hold_btn == btn2;

  set_button(btn1, btn1_label, &btn1_path, s->command_busy ? "..." : h1 ? "KEEP HOLDING" : "RELEARN THE SEA",
             "", 182, 300, 58, h1 ? COL_ORANGE : COL_BLUE, h1 ? lv_color_hex(0x221400) : COL_TEXT);
  set_button(btn2, btn2_label, &btn2_path, h2 ? "KEEP HOLDING" : "NEW PULL DIRECTION",
             "", 258, 300, 58, h2 ? COL_ORANGE : COL_BLUE, h2 ? lv_color_hex(0x221400) : COL_TEXT);

  if(imu_present())
    set_button(btn3, btn3_label, &btn3_path, "TEACH MY LOOK", "#teach", 336, 250, 52, COL_BTN, COL_TEXT);
}

static void time_left(char *out, size_t size, int pct, long minutes)
{
  if(pct < 0)
    snprintf(out, size, "--");
  else if(minutes < 0)
    snprintf(out, size, "measuring...");
  else
    snprintf(out, size, "%ld h %02ld m", minutes / 60, minutes % 60);
}

static void battery_row(int r, int pct, long minutes)
{
  char a[24];

  lv_color_t c = pct < 0 ? COL_DIM : pct < 20 ? COL_RED : pct < 50 ? COL_ORANGE : lv_color_hex(0x3ddc84);

  if(pct < 0)
    snprintf(a, sizeof(a), "--");
  else
    snprintf(a, sizeof(a), "%d%%", pct);

  set_text(bat_pct[r], a);
  set_text_color(bat_pct[r], c);
  lv_bar_set_value(bat_bar[r], pct < 0 ? 0 : pct, LV_ANIM_OFF);

  if(lv_obj_get_style_bg_color(bat_bar[r], LV_PART_INDICATOR).full != c.full)
    lv_obj_set_style_bg_color(bat_bar[r], c, LV_PART_INDICATOR);

  time_left(a, sizeof(a), pct, minutes);
  set_text(bat_left[r], pct < 0 ? "not measured" : a);
  set_text(bat_leftcap[r], pct < 0 || minutes < 0 ? "" : "left");
}

static void battery_page(const ble_link_status_t *s)
{
  layout(MODE_BATTERY);
  ring_set(0, COL_CYAN);

  set_text(title, "BATTERY");
  set_text_color(title, COL_TEXT);
  set_text(subtitle, "");
  set_text(hint, "swipe down: back");

  battery_row(0, s->bat, s->batleft);
  battery_row(1, watch_bat_percent(), watch_bat_left_min());
}

static void last_strike_page(const ble_link_status_t *s, uint32_t now)
{
  layout(MODE_LASTSTRIKE);
  ring_set(0, COL_RED);

  set_text(title, "LAST STRIKE");
  set_text_color(title, COL_RED);
  set_text(hint, "swipe up: back");

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

  if(!got || ls.error)
  {
    if(!got)
      set_text(subtitle, now - ls_entered > 6000 ? "the rod does not answer" : "loading...");
    else
      set_text(subtitle, ls.error == 404 ? "update the rod sketch" : "the rod said no");

    set_text_color(subtitle, COL_TEXT);
    show(ls_chart, false);
    show(ls_mark, false);
    show(ls_mark_label, false);
    set_text(ls_info[0], "");
    set_text(ls_info[1], got && ls.error == 404 ? "(it has no last strike yet:\nupload the newest rod sketch)" : "");
    set_text(ls_info[2], "");
    return;
  }

  bool graph = ls.have && ls.n > 1;
  show(ls_chart, graph);
  show(ls_mark, graph);
  show(ls_mark_label, graph);

  char a[64];

  if(!ls.have)
  {
    set_text(subtitle, "No strike yet");
    set_text_color(subtitle, COL_TEXT);
    set_text(ls_info[0], "");
    set_text(ls_info[1], "since the rod was switched on");
    set_text(ls_info[2], "");
    return;
  }

  // how long ago, counting on from when it was fetched
  long ago = ls.ago_s + (long)((now - ls.got_ms) / 1000);

  if(ago < 60)
    snprintf(a, sizeof(a), "%s  -  %ld s ago", ls.type[0] ? ls.type : "STRIKE", ago);
  else if(ago < 3600)
    snprintf(a, sizeof(a), "%s  -  %ld min ago", ls.type[0] ? ls.type : "STRIKE", ago / 60);
  else
    snprintf(a, sizeof(a), "%s  -  %ld h %02ld min ago", ls.type[0] ? ls.type : "STRIKE", ago / 3600, (ago % 3600) / 60);

  set_text(subtitle, a);
  set_text_color(subtitle, COL_TEXT);

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

    // red line where the strike was (in the chart's drawing area)
    int x = n > 1 ? pre * (360 - 16) / (n - 1) : 0;
    lv_obj_set_pos(ls_mark, x, 0);
    lv_obj_set_pos(ls_mark_label, x + 5 > 360 - 16 - 60 ? x - 62 : x + 5, 0);
  }

  if(zoom)
    snprintf(a, sizeof(a), "ZOOM  %.1f s before | %.1f s after  (tap)", pre / 50.0f, (n - pre) / 50.0f);
  else
    snprintf(a, sizeof(a), "%d s before | %d s after  (tap: zoom)", pre / 5, (n - pre) / 5);

  set_text(ls_info[0], a);
  snprintf(a, sizeof(a), "tilt peak %+.1f     pull %d%%", ls.tiltpeak, ls.pull);
  set_text(ls_info[1], a);

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
    spinning = true;
    lv_arc_set_rotation(ring, spin);
    lv_arc_set_value(ring, 25);

    if(lv_obj_get_style_arc_color(ring, LV_PART_INDICATOR).full != COL_BLUE.full)
      lv_obj_set_style_arc_color(ring, COL_BLUE, LV_PART_INDICATOR);

    set_text(title, LV_SYMBOL_BLUETOOTH "\nLOOKING FOR\nTHE ROD");
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
    if(s.strikeid != strike_quiet && now - buzz_strike_at >= 1500)
    {
      buzz_strike_at = now;
      buzz_play(BUZZ_STRIKE);
    }
  }
  else
  {
    buzz_strike_at = 0;
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
    layout(MODE_STRIKE);

    bool on = (now / 350) % 2 == 0;     // flashing

    set_bg_color(scr, on ? lv_color_hex(0x5a000c) : lv_color_hex(0x220004));
    ring_set(100, on ? COL_RED : lv_color_hex(0x7a0010));

    set_text(title, "STRIKE!");
    set_text_color(title, COL_TEXT);
    set_text(subtitle, s.type[0] ? s.type : "FISH ON");
    set_text_color(subtitle, COL_RED);
    set_font(subtitle, FONT_MID);

    set_button(btn1, btn1_label, &btn1_path, s.command_busy ? "..." : "I'M WATCHING", "/watching", 290, 300, 66, COL_CYAN, lv_color_hex(0x001226));
    set_button(btn2, btn2_label, &btn2_path, "REEL IN", "/reelin", 372, 300, 66, COL_TEXT, lv_color_hex(0x7a0010));
    return;
  }

  set_font(subtitle, FONT_SMALL);

  if(strcmp(s.state, "paused") == 0)
  {
    layout(MODE_REELED);
    ring_set(0, COL_CYAN);

    set_text(title, "REELED IN");
    set_text_color(title, COL_TEXT);
    set_text(subtitle, "cast, then press READY");
    set_text_color(subtitle, COL_DIM);

    set_button(btn1, btn1_label, &btn1_path, s.command_busy ? "..." : "READY", "/ready", 330, 260, 78, COL_BLUE, COL_TEXT);
    return;
  }

  if(strcmp(s.state, "pulltest") == 0)
  {
    layout(MODE_PULLTEST);

    float frac = s.ptneed > 0 ? s.ptdeg / s.ptneed : 0;
    frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);

    ring_set((int)(frac * 100), COL_ORANGE);

    set_text(title, "PULL TEST");
    set_text_color(title, COL_ORANGE);

    set_text(subtitle, frac >= 1 ? "good - now let go" : "pull the line gently\ntoward the sea, then let go");
    set_text_color(subtitle, COL_TEXT);

    lv_bar_set_value(bar, (int)(frac * 100), LV_ANIM_OFF);

    set_button(btn1, btn1_label, &btn1_path, s.command_busy ? "..." : "SKIP", "/skippull", 360, 170, 54, COL_BTN, COL_TEXT);
    return;
  }

  // ---------- live ----------
  layout(MODE_LIVE);

  lv_color_t c = COL_CYAN;
  bool warn = false;              // orange: something building
  int ringv = s.bucket;

  if(strcmp(s.state, "learning") == 0)
  {
    set_text(title, "LEARNING");
    set_text(subtitle, "keep the rod still");
    c = COL_ORANGE;
    warn = true;
  }
  else if(strcmp(s.state, "settling") == 0)
  {
    set_text(title, "SETTLING");
    set_text(subtitle, "learning the new position");
    c = COL_ORANGE;
    warn = true;
  }
  else if(s.dropwatch)
  {
    set_text(title, "DROP-BACK");
    set_text(subtitle, "watching for a pull...");
    c = COL_ORANGE;
    warn = true;
    ringv = 100;
  }
  else if(s.bucket >= PULL_SHOW)
  {
    set_text(title, "SLOW PULL");
    snprintf(a, sizeof(a), "%d%% - building up", s.bucket);
    set_text(subtitle, a);
    c = COL_ORANGE;
    warn = true;
  }
  else if(s.drop >= PULL_SHOW)
  {
    set_text(title, "DROP-BACK");
    snprintf(a, sizeof(a), "%d%% - line going slack", s.drop);
    set_text(subtitle, a);
    c = COL_ORANGE;
    warn = true;
    ringv = s.drop;
  }
  else if(now < knock_until)
  {
    set_text(title, "KNOCK");
    snprintf(a, sizeof(a), "%d knock%s in the last 4 s", s.knocks, s.knocks == 1 ? "" : "s");
    set_text(subtitle, a);
  }
  else
  {
    set_text(title, "ROD ARMED");
    snprintf(a, sizeof(a), "waves %s - wind %s", wave_name(s.wave), wind_name(s.wind));
    set_text(subtitle, s.dir ? a : "no pull test: knocks only");
  }

  set_text_color(title, c);
  set_text_color(subtitle, COL_DIM);
  ring_set(ringv < 4 ? 4 : ringv, c);

  set_text(cap1, "TILT (DEG)");          // the built-in fonts have no degree sign
  snprintf(a, sizeof(a), "%+.1f", s.tilt);
  set_text(val1, a);
  set_text_color(val1, warn ? COL_ORANGE : COL_VIOLET);

  if(s.bucket >= PULL_SHOW)
  {
    set_text(cap2, "PULL");
    snprintf(b, sizeof(b), "%d%%", s.bucket);
    set_text_color(val2, COL_ORANGE);
  }
  else
  {
    set_text(cap2, "KNOCKS");
    snprintf(b, sizeof(b), "%d", s.knocks);
    set_text_color(val2, s.knocks > 0 ? COL_CYAN : COL_TEXT);
  }

  set_text(val2, b);

  if(strcmp(s.state, "learning") != 0)
    set_button(btn1, btn1_label, &btn1_path, s.command_busy ? "..." : "REEL IN", "/reelin", 405, 170, 50, COL_BTN, COL_TEXT);
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
}

// ---------- building the screen ----------

void watch_ui_create(void)
{
  scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // outer ring
  ring = lv_arc_create(scr);
  lv_obj_set_size(ring, 452, 452);
  lv_obj_center(ring);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_arc_set_range(ring, 0, 100);
  lv_arc_set_rotation(ring, 270);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(ring, 10, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ring, COL_RING_BG, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, 10, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(ring, true, LV_PART_INDICATOR);

  // top: Bluetooth bars, rod battery
  bt_icon = label(scr, FONT_TINY, COL_CYAN);
  lv_label_set_text(bt_icon, LV_SYMBOL_BLUETOOTH);
  lv_obj_align(bt_icon, LV_ALIGN_TOP_MID, -80, 44);

  for(int i = 0; i < 4; i++)
  {
    bars[i] = lv_obj_create(scr);
    lv_obj_remove_style_all(bars[i]);
    lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, 0);
    lv_obj_set_size(bars[i], 5, 6 + i * 5);
    lv_obj_align(bars[i], LV_ALIGN_TOP_MID, -62 + i * 8, 64 - (6 + i * 5));
  }

  bat_label = label(scr, FONT_TINY, COL_DIM);
  lv_obj_align(bat_label, LV_ALIGN_TOP_MID, 40, 44);

  // title + subtitle
  title = label(scr, FONT_BIG, COL_CYAN);
  subtitle = label(scr, FONT_SMALL, COL_DIM);

  // live graph
  chart = lv_chart_create(scr);
  lv_obj_set_size(chart, 340, 112);
  lv_obj_align(chart, LV_ALIGN_TOP_MID, 0, 178);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, GRAPH_POINTS);
  lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(chart, 0, 0);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -300, 300);     // knock 0..300 = top half
  lv_chart_set_range(chart, LV_CHART_AXIS_SECONDARY_Y, -300, 900);   // tilt -3..+3 deg = bottom half
  lv_obj_set_style_bg_color(chart, COL_PANEL, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_radius(chart, 14, 0);
  lv_obj_set_style_pad_all(chart, 8, 0);
  lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);           // no dots, just lines
  lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);

  ser_limit = lv_chart_add_series(chart, lv_color_hex(0x6b5426), LV_CHART_AXIS_PRIMARY_Y);
  ser_tilt = lv_chart_add_series(chart, COL_VIOLET, LV_CHART_AXIS_SECONDARY_Y);
  ser_knock = lv_chart_add_series(chart, COL_CYAN, LV_CHART_AXIS_PRIMARY_Y);

  lv_chart_set_all_value(chart, ser_limit, KNOCK_LIMIT);
  lv_chart_set_all_value(chart, ser_tilt, 0);
  lv_chart_set_all_value(chart, ser_knock, 0);

  chart_knock_label = label(chart, FONT_TINY, COL_CYAN);
  lv_label_set_text(chart_knock_label, "KNOCK");
  lv_obj_align(chart_knock_label, LV_ALIGN_TOP_LEFT, 4, 0);

  chart_tilt_label = label(chart, FONT_TINY, COL_VIOLET);
  lv_label_set_text(chart_tilt_label, "TILT");
  lv_obj_align(chart_tilt_label, LV_ALIGN_BOTTOM_LEFT, 4, 0);

  // two numbers under the graph
  cap1 = label(scr, FONT_TINY, COL_DARK);
  lv_obj_align(cap1, LV_ALIGN_TOP_MID, -83, 304);
  val1 = label(scr, FONT_MID, COL_VIOLET);
  lv_obj_align(val1, LV_ALIGN_TOP_MID, -83, 324);

  cap2 = label(scr, FONT_TINY, COL_DARK);
  lv_obj_align(cap2, LV_ALIGN_TOP_MID, 83, 304);
  val2 = label(scr, FONT_MID, COL_TEXT);
  lv_obj_align(val2, LV_ALIGN_TOP_MID, 83, 324);

  // pull test bar
  bar = lv_bar_create(scr);
  lv_bar_set_range(bar, 0, 100);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x262b36), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, COL_ORANGE, LV_PART_INDICATOR);

  // sea state page: two rows (name + 4 level blocks)
  const char *sea_caps[2] = { "WAVES", "WIND" };

  for(int r = 0; r < 2; r++)
  {
    sea_cap[r] = label(scr, FONT_TINY, COL_DIM);
    lv_label_set_text(sea_cap[r], sea_caps[r]);
    lv_obj_align(sea_cap[r], LV_ALIGN_TOP_LEFT, 92, 176 + r * 86);

    sea_val[r] = label(scr, FONT_MID, COL_CYAN);
    lv_obj_align(sea_val[r], LV_ALIGN_TOP_LEFT, 92, 198 + r * 86);

    for(int i = 0; i < 4; i++)
    {
      sea_bar[r][i] = lv_obj_create(scr);
      lv_obj_remove_style_all(sea_bar[r][i]);
      lv_obj_set_style_bg_opa(sea_bar[r][i], LV_OPA_COVER, 0);
      lv_obj_set_style_radius(sea_bar[r][i], 5, 0);
      lv_obj_set_size(sea_bar[r][i], 30, 12);
      lv_obj_align(sea_bar[r][i], LV_ALIGN_TOP_LEFT, 236 + i * 36, 182 + r * 86);
    }
  }

  // relearn page: small text under the button (also "swipe ...: back")
  hint = label(scr, FONT_TINY, COL_DIM);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 350);

  // battery page: ROD and WATCH rows
  const char *bat_names[2] = { "ROD", "WATCH" };

  for(int r = 0; r < 2; r++)
  {
    int y = 104 + r * 140;

    bat_cap[r] = label(scr, FONT_TINY, COL_DIM);
    lv_label_set_text(bat_cap[r], bat_names[r]);
    lv_obj_align(bat_cap[r], LV_ALIGN_TOP_LEFT, 72, y);

    bat_pct[r] = label(scr, FONT_HUGE, COL_TEXT);
    lv_obj_align(bat_pct[r], LV_ALIGN_TOP_LEFT, 70, y + 22);

    bat_leftcap[r] = label(scr, FONT_TINY, COL_DIM);
    lv_obj_align(bat_leftcap[r], LV_ALIGN_TOP_RIGHT, -72, y + 10);

    bat_left[r] = label(scr, FONT_SMALL, COL_TEXT);
    lv_obj_align(bat_left[r], LV_ALIGN_TOP_RIGHT, -72, y + 34);

    bat_bar[r] = lv_bar_create(scr);
    lv_bar_set_range(bat_bar[r], 0, 100);
    lv_obj_set_size(bat_bar[r], 322, 14);
    lv_obj_align(bat_bar[r], LV_ALIGN_TOP_MID, 0, y + 84);
    lv_obj_set_style_bg_color(bat_bar[r], lv_color_hex(0x262b36), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bat_bar[r], LV_OPA_COVER, LV_PART_MAIN);
  }

  // last strike page: graph of the rod's recording
  ls_chart = lv_chart_create(scr);
  lv_obj_set_size(ls_chart, 360, 150);
  lv_obj_align(ls_chart, LV_ALIGN_TOP_MID, 0, 138);
  lv_chart_set_type(ls_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(ls_chart, LS_MAX);
  lv_chart_set_div_line_count(ls_chart, 0, 0);
  lv_chart_set_range(ls_chart, LV_CHART_AXIS_PRIMARY_Y, -300, 300);
  lv_chart_set_range(ls_chart, LV_CHART_AXIS_SECONDARY_Y, -300, 900);
  lv_obj_set_style_bg_color(ls_chart, COL_PANEL, 0);
  lv_obj_set_style_border_width(ls_chart, 0, 0);
  lv_obj_set_style_radius(ls_chart, 14, 0);
  lv_obj_set_style_pad_all(ls_chart, 8, 0);
  lv_obj_set_style_size(ls_chart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(ls_chart, 3, LV_PART_ITEMS);
  lv_obj_add_flag(ls_chart, LV_OBJ_FLAG_CLICKABLE);              // tap: ZOOM <-> 30 s
  lv_obj_clear_flag(ls_chart, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(ls_chart, ls_chart_clicked, LV_EVENT_CLICKED, NULL);

  ls_limit = lv_chart_add_series(ls_chart, lv_color_hex(0x6b5426), LV_CHART_AXIS_PRIMARY_Y);
  ls_tilt = lv_chart_add_series(ls_chart, COL_VIOLET, LV_CHART_AXIS_SECONDARY_Y);
  ls_knock = lv_chart_add_series(ls_chart, COL_CYAN, LV_CHART_AXIS_PRIMARY_Y);

  // red strike line + label: inside the graph (can never stick out of it)
  ls_mark = lv_obj_create(ls_chart);
  lv_obj_remove_style_all(ls_mark);
  lv_obj_add_flag(ls_mark, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_clear_flag(ls_mark, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(ls_mark, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(ls_mark, COL_RED, 0);
  lv_obj_set_size(ls_mark, 2, 150 - 16);

  ls_mark_label = label(ls_chart, FONT_TINY, COL_RED);
  lv_obj_add_flag(ls_mark_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_label_set_text(ls_mark_label, "STRIKE");

  for(int i = 0; i < 3; i++)
  {
    ls_info[i] = label(scr, i == 1 ? FONT_SMALL : FONT_TINY, i == 1 ? COL_TEXT : COL_DIM);
    lv_obj_align(ls_info[i], LV_ALIGN_TOP_MID, 0, 296 + i * 30 + (i == 2 ? 4 : 0));
  }

  // page dots at the bottom
  for(int i = 0; i < 3; i++)
  {
    dots[i] = lv_obj_create(scr);
    lv_obj_remove_style_all(dots[i]);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(dots[i], 10, 10);
    lv_obj_align(dots[i], LV_ALIGN_TOP_MID, -20 + i * 20, 438);
  }

  // buttons
  btn1 = button(&btn1_label, &btn1_path);
  btn2 = button(&btn2_label, &btn2_path);
  btn3 = button(&btn3_label, &btn3_path);

  // boot logo on top of everything for the first seconds
  boot_img = lv_img_create(scr);
  lv_img_set_src(boot_img, &strikesense_boot_fixed);
  lv_obj_center(boot_img);

  started_ms = lv_tick_get();
  mode = MODE_LIVE;               // force the first layout
  layout(MODE_BOOT);

  lv_timer_create(ui_timer, 100, NULL);
  lv_timer_create(graph_timer, 100, NULL);
  lv_timer_create(swipe_timer, 20, NULL);
}

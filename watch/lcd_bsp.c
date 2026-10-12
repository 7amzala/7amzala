#include "lcd_bsp.h"
#include "strikesense_boot_fixed.h"
#include "esp_lcd_sh8601.h"
#include "lcd_config.h"
#include "FT3168.h"
#include "read_lcd_id_bsp.h"
static SemaphoreHandle_t lvgl_mux = NULL; //mutex semaphores
#define LCD_HOST    SPI2_HOST

#define EXAMPLE_Rotate_90
#define SH8601_ID 0x86
#define CO5300_ID 0xff
static uint8_t READ_LCD_ID = 0x00;



// =====================================================
// COMPLETE FISHING WATCH PRODUCT SIMULATION
// =====================================================
// Slightly larger UI fonts.
// Make sure LV_FONT_MONTSERRAT_16 and LV_FONT_MONTSERRAT_20 are enabled in lv_conf.h.
#define FONT_UI       (&lv_font_montserrat_16)
#define FONT_TITLE    (&lv_font_montserrat_16)
#define FONT_BIG      (&lv_font_montserrat_20)

//
// This is still a DISPLAY/TOUCH simulation.
// Later the fake connection, RSSI, rod battery and graph values
// will be replaced by real SX1262 LoRa packets and flex-sensor data.
//
// Suggested future LoRa protocol:
//   WATCH -> ROD : HELLO, START_LEARN, CAST_MODE, PAUSE, RESUME
//   ROD   -> WATCH: HELLO_ACK, HEARTBEAT, SENSOR_SAMPLE,
//                  LEARN_DONE, BATTERY, STRIKE
//
// A HEARTBEAT every ~1 second lets the watch detect link loss.
// =====================================================

#define DEMO_AUTO_CONNECT 1

// Set to 1 only when you want short timers while developing.
// Default 0 gives the real requested 2-minute learn + 50-second cast.
#define DEMO_FAST_TIMERS 0

#if DEMO_FAST_TIMERS
  #define LEARN_DURATION_MS   12000UL
  #define CAST_DURATION_MS    10000UL
#else
  #define LEARN_DURATION_MS   20000UL
  #define CAST_DURATION_MS    50000UL
#endif

#define BOOT_DURATION_MS       5000UL
#define CONNECT_DURATION_MS    3500UL
#define CONNECT_FAIL_MS        8000UL
#define CONNECTED_HOLD_MS      1500UL
#define SETTLE_DURATION_MS     3500UL

typedef enum
{
  APP_BOOT = 0,
  APP_CONNECTING,
  APP_CONNECT_FAILED,
  APP_CONNECTED,
  APP_LEARN_PROMPT,
  APP_LEARNING,
  APP_LEARN_COMPLETE,
  APP_CAST_READY,
  APP_CAST_COUNTDOWN,
  APP_SETTLING,
  APP_MONITORING,
  APP_STRIKE,
  APP_REEL_PAUSED,
  APP_ADJUST
} app_state_t;

typedef enum
{
  ACT_NONE = 0,
  ACT_RETRY_CONNECT,
  ACT_START_LEARN,
  ACT_READY_TO_CAST,
  ACT_READY_NOW,
  ACT_SIM_STRIKE,
  ACT_WATCHING,
  ACT_REEL_IN,
  ACT_HISTORY_BACK,
  ACT_ADJUST_ROD,
  ACT_ADJUST_READY
} app_action_t;

static lv_obj_t *app_screen;

static lv_obj_t *title_label;
static lv_obj_t *subtitle_label;
static lv_obj_t *big_label;
static lv_obj_t *detail_label;

static lv_obj_t *rod_batt_box;
static lv_obj_t *watch_batt_box;
static lv_obj_t *rod_batt_label;
static lv_obj_t *watch_batt_label;

static lv_obj_t *main_chart;
static lv_chart_series_t *main_series;
static lv_obj_t *value_label;
static lv_obj_t *base_label;
static lv_obj_t *change_label;
static lv_obj_t *progress_bar;

static lv_obj_t *signal_bars[4];

static app_state_t app_state = APP_BOOT;
static app_state_t signal_return_state = APP_MONITORING;

static bool rod_connected = false;
static bool session_learned = false;
static bool signal_page_visible = false;
static bool strike_running = false;

// ---- One-rod strike recorder / history ----
#define STRIKE_HISTORY_SAMPLES 72
static int strike_history[STRIKE_HISTORY_SAMPLES];
static int strike_history_count = 0;
static bool strike_history_valid = false;
static bool strike_event_complete = false;
static uint32_t last_strike_ms = 0;
static uint32_t strike_event_started_ms = 0;
static uint32_t strike_duration_ms = 0;
static int strike_peak_count = 0;
static int strike_peak_delta = 0;
static bool history_page_visible = false;
static int strike_sim_step = -1;

static uint32_t state_started_ms = 0;
static uint32_t last_graph_ms = 0;
static uint32_t last_signal_ms = 0;

static int baseline_value = 2000;
static int current_value = 2000;
static int strike_step = 0;
static int calm_step = 0;
static int signal_demo_step = 0;
static uint8_t signal_level = 4;

static const int strike_pattern[] =
{
  0, 2, 5, 9, 15,
  24, 38, 58, 85,
  120, 165, 215,
  275, 335, 390,
  430, 455, 475,
  480, 472, 460,
  448, 438, 430
};

static const int strike_pattern_length =
  sizeof(strike_pattern) / sizeof(strike_pattern[0]);

static void set_label_font(lv_obj_t *label, const lv_font_t *font)
{
  if (label != NULL) {
    lv_obj_set_style_text_font(label, font, 0);
  }
}

static uint32_t app_now_ms(void)
{
  return lv_tick_get();
}

static uint32_t app_elapsed_ms(void)
{
  return app_now_ms() - state_started_ms;
}

static void app_set_state(app_state_t new_state);
static void app_button_event(lv_event_t *e);
static void app_gesture_event(lv_event_t *e);
static void app_render_current(void);
static void app_render_signal_page(void);

static void set_screen_bg(uint32_t hex)
{
  lv_obj_set_style_bg_color(app_screen, lv_color_hex(hex), 0);
  lv_obj_set_style_bg_opa(app_screen, LV_OPA_COVER, 0);
}

static lv_obj_t *make_panel(
  lv_obj_t *parent,
  int width,
  int height,
  int y,
  uint32_t bg,
  uint32_t border,
  int radius)
{
  lv_obj_t *panel = lv_obj_create(parent);

  lv_obj_set_size(panel, width, height);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, y);

  lv_obj_set_style_radius(panel, radius, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(border), 0);
  lv_obj_set_style_pad_all(panel, 0, 0);

  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  return panel;
}

static lv_obj_t *make_button(
  const char *text,
  int x,
  int y,
  int width,
  int height,
  uint32_t bg,
  uint32_t fg,
  app_action_t action)
{
  lv_obj_t *button = lv_btn_create(app_screen);

  lv_obj_set_size(button, width, height);
  lv_obj_align(button, LV_ALIGN_BOTTOM_MID, x, y);

  lv_obj_set_style_radius(button, height / 2, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(bg), 0);
  lv_obj_set_style_shadow_width(button, 0, 0);

  lv_obj_t *label = lv_label_create(button);
set_label_font(label, FONT_UI);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(fg), 0);
  lv_obj_center(label);

  lv_obj_add_event_cb(
    button,
    app_button_event,
    LV_EVENT_CLICKED,
    (void *)(intptr_t)action
  );

  return button;
}

static lv_obj_t *make_battery_chip(
  const char *text,
  int x_offset,
  bool dimmed)
{
  lv_obj_t *box = lv_obj_create(app_screen);

  lv_obj_set_size(box, 105, 30);
  lv_obj_align(box, LV_ALIGN_TOP_MID, x_offset, 36);

  lv_obj_set_style_radius(box, 15, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_bg_color(
    box,
    lv_color_hex(dimmed ? 0x0E1116 : 0x11151B),
    0
  );
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *label = lv_label_create(box);
set_label_font(label, FONT_UI);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(
    label,
    lv_color_hex(dimmed ? 0x6E7782 : 0xD6DBE1),
    0
  );
  lv_obj_center(label);

  return box;
}

static void add_connected_batteries(void)
{
  rod_batt_box = make_battery_chip("ROD 92%", -88, false);
  watch_batt_box = make_battery_chip("WATCH 86%", 88, false);
}

static void add_watch_battery_only(void)
{
  watch_batt_box = make_battery_chip("WATCH 86%", 0, false);
}

static void add_header(
  const char *title,
  const char *subtitle,
  uint32_t title_color,
  uint32_t subtitle_color,
  bool show_rod_battery)
{
  if (show_rod_battery) {
    add_connected_batteries();
  }
  else {
    add_watch_battery_only();
  }

  title_label = lv_label_create(app_screen);


  set_label_font(title_label, FONT_TITLE);
  lv_label_set_text(title_label, title);
  lv_obj_set_style_text_color(title_label, lv_color_hex(title_color), 0);
  lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 80);

  subtitle_label = lv_label_create(app_screen);


  set_label_font(subtitle_label, FONT_UI);
  lv_label_set_text(subtitle_label, subtitle);
  lv_obj_set_style_text_color(subtitle_label, lv_color_hex(subtitle_color), 0);
  lv_obj_align(subtitle_label, LV_ALIGN_TOP_MID, 0, 106);
}

static void add_center_message(
  const char *main_text,
  const char *detail_text,
  uint32_t main_color,
  uint32_t detail_color)
{
  big_label = lv_label_create(app_screen);

  set_label_font(big_label, FONT_BIG);
  lv_label_set_text(big_label, main_text);
  lv_obj_set_style_text_color(big_label, lv_color_hex(main_color), 0);
  lv_obj_align(big_label, LV_ALIGN_CENTER, 0, -15);

  detail_label = lv_label_create(app_screen);


  set_label_font(detail_label, FONT_UI);
  lv_label_set_text(detail_label, detail_text);
  lv_obj_set_style_text_color(detail_label, lv_color_hex(detail_color), 0);
  lv_obj_set_width(detail_label, 300);
  lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(detail_label, LV_ALIGN_CENTER, 0, 20);
}

static void add_progress(uint32_t max_value)
{
  progress_bar = lv_bar_create(app_screen);

  lv_obj_set_size(progress_bar, 250, 12);
  lv_obj_align(progress_bar, LV_ALIGN_CENTER, 0, 72);

  lv_obj_set_style_radius(progress_bar, 6, 0);
  lv_obj_set_style_bg_color(
    progress_bar,
    lv_color_hex(0x202731),
    LV_PART_MAIN
  );

  lv_obj_set_style_bg_color(
    progress_bar,
    lv_color_hex(0x38BDF8),
    LV_PART_INDICATOR
  );

  lv_bar_set_range(progress_bar, 0, max_value);
  lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
}

static void add_chart_panel(bool learning_mode)
{
  lv_obj_t *panel = make_panel(
    app_screen,
    340,
    learning_mode ? 165 : 205,
    learning_mode ? 18 : 4,
    0x0D1117,
    0x252B35,
    24
  );

  lv_obj_t *chart_title = lv_label_create(panel);
set_label_font(chart_title, FONT_UI);
  lv_label_set_text(chart_title, learning_mode ? "ENVIRONMENT MOVEMENT" : "LIVE BEND");
  lv_obj_set_style_text_color(chart_title, lv_color_hex(0x7E8795), 0);
  lv_obj_align(chart_title, LV_ALIGN_TOP_LEFT, 20, 13);

  value_label = lv_label_create(panel);


  set_label_font(value_label, FONT_TITLE);
  lv_label_set_text(value_label, "2000");
  lv_obj_set_style_text_color(value_label, lv_color_hex(0xF5F7FA), 0);
  lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -20, 13);

  main_chart = lv_chart_create(panel);
  lv_obj_set_size(
    main_chart,
    300,
    learning_mode ? 105 : 112
  );
  lv_obj_align(
    main_chart,
    LV_ALIGN_CENTER,
    0,
    learning_mode ? 18 : 3
  );

  lv_obj_set_style_bg_opa(main_chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(main_chart, 0, 0);
  lv_obj_set_style_pad_all(main_chart, 0, 0);

  lv_chart_set_type(main_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(main_chart, 70);
  lv_chart_set_range(
    main_chart,
    LV_CHART_AXIS_PRIMARY_Y,
    1850,
    2550
  );
  lv_chart_set_div_line_count(main_chart, 3, 5);

  main_series = lv_chart_add_series(
    main_chart,
    lv_color_hex(0x38BDF8),
    LV_CHART_AXIS_PRIMARY_Y
  );

  for (int i = 0; i < 70; i++) {
    lv_chart_set_next_value(main_chart, main_series, baseline_value);
  }

  if (!learning_mode) {
    base_label = lv_label_create(panel);

    set_label_font(base_label, FONT_UI);
    lv_label_set_text(base_label, "BASE 2000");
    lv_obj_set_style_text_color(base_label, lv_color_hex(0x8C96A3), 0);
    lv_obj_align(base_label, LV_ALIGN_BOTTOM_LEFT, 20, -13);

    change_label = lv_label_create(panel);


    set_label_font(change_label, FONT_UI);
    lv_label_set_text(change_label, "+0");
    lv_obj_set_style_text_color(change_label, lv_color_hex(0x38BDF8), 0);
    lv_obj_align(change_label, LV_ALIGN_BOTTOM_RIGHT, -20, -13);
  }
}

static void update_monitor_values(void)
{
  char buf[32];

  if (value_label != NULL) {
    snprintf(buf, sizeof(buf), "%d", current_value);
    lv_label_set_text(value_label, buf);
  }

  if (base_label != NULL) {
    snprintf(buf, sizeof(buf), "BASE %d", baseline_value);
    lv_label_set_text(base_label, buf);
  }

  if (change_label != NULL) {
    snprintf(buf, sizeof(buf), "%+d", current_value - baseline_value);
    lv_label_set_text(change_label, buf);
  }
}

static void add_gesture_handler(void)
{
  if (!rod_connected) {
    return;
  }

  lv_obj_add_event_cb(
    app_screen,
    app_gesture_event,
    LV_EVENT_GESTURE,
    NULL
  );
}

static void render_boot(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x000000);

  // Full-screen Laarif StrikeSense brand image
  lv_obj_t *boot_img = lv_img_create(app_screen);
  lv_img_set_src(boot_img, &strikesense_boot_fixed);
  lv_obj_center(boot_img);

  // Bottom readability panel
  lv_obj_t *bottom_panel = lv_obj_create(app_screen);
  lv_obj_set_size(bottom_panel, 390, 138);
  lv_obj_align(bottom_panel, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_set_style_bg_color(bottom_panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(bottom_panel, 190, 0);
  lv_obj_set_style_border_width(bottom_panel, 0, 0);
  lv_obj_set_style_radius(bottom_panel, 28, 0);
  lv_obj_clear_flag(bottom_panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *byline = lv_label_create(bottom_panel);
  set_label_font(byline, FONT_TITLE);
  lv_label_set_text(byline, "by Hamza Laarif");
  lv_obj_set_style_text_color(byline, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(byline, LV_ALIGN_TOP_MID, 0, 12);

  lv_obj_t *status = lv_label_create(bottom_panel);
  set_label_font(status, FONT_UI);
  lv_label_set_text(status, "Starting system...");
  lv_obj_set_style_text_color(status, lv_color_hex(0x38D9FF), 0);
  lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 50);

  lv_obj_t *spinner = lv_spinner_create(bottom_panel, 900, 75);
  lv_obj_set_size(spinner, 46, 46);
  lv_obj_align(spinner, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_arc_width(spinner, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(0x1B2735), LV_PART_MAIN);
  lv_obj_set_style_arc_width(spinner, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(0x38D9FF), LV_PART_INDICATOR);
}

static void render_connecting(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x000000);

  add_header(
    "CONNECTING TO ROD",
    "Please wait...",
    0xF5F7FA,
    0x8C96A3,
    false
  );

  lv_obj_t *panel = make_panel(
    app_screen,
    300,
    150,
    15,
    0x0D1117,
    0x252B35,
    26
  );

  lv_obj_t *search = lv_label_create(panel);
set_label_font(search, FONT_UI);
  lv_label_set_text(search, "SEARCHING");
  lv_obj_set_style_text_color(search, lv_color_hex(0x38BDF8), 0);
  lv_obj_align(search, LV_ALIGN_CENTER, 0, -18);

  detail_label = lv_label_create(panel);


  set_label_font(detail_label, FONT_UI);
  lv_label_set_text(detail_label, "Looking for rod unit...");
  lv_obj_set_style_text_color(detail_label, lv_color_hex(0x7E8795), 0);
  lv_obj_align(detail_label, LV_ALIGN_CENTER, 0, 16);
}

static void render_connect_failed(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x160606);

  add_header(
    "ROD NOT FOUND",
    "Connection pending",
    0xFFFFFF,
    0xFFB8B8,
    false
  );

  add_center_message(
    "NO LINK",
    "Check that the rod unit is powered on and nearby.",
    0xEF4444,
    0xD8A5A5
  );

  make_button(
    "RETRY",
    0,
    -42,
    180,
    50,
    0x7A1212,
    0xFFFFFF,
    ACT_RETRY_CONNECT
  );
}

static void render_connected(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x03160B);

  add_header(
    "ROD CONNECTED",
    "Link established",
    0xFFFFFF,
    0x4ADE80,
    true
  );

  add_center_message(
    "CONNECTED",
    "Signal GOOD\nRod battery 92%",
    0x4ADE80,
    0xC7F7D7
  );

  add_gesture_handler();
}

static void render_learn_prompt(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x000000);

  add_header(
    "ENVIRONMENT SETUP",
    "First setup for this fishing session",
    0xF5F7FA,
    0x8C96A3,
    true
  );

  lv_obj_t *panel = make_panel(
    app_screen,
    330,
    175,
    5,
    0x0D1117,
    0x252B35,
    24
  );

  lv_obj_t *instruction = lv_label_create(panel);
set_label_font(instruction, FONT_UI);
  lv_label_set_text(
    instruction,
    "1. Cast WITHOUT bait\n"
    "2. Tighten the line normally\n"
    "3. Place the rod on the rest\n"
    "4. Keep the rod untouched"
  );
  lv_obj_set_style_text_color(instruction, lv_color_hex(0xD6DBE1), 0);
  lv_obj_set_width(instruction, 275);
  lv_obj_set_style_text_align(instruction, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_center(instruction);

  make_button(
    "START LEARNING",
    0,
    -38,
    220,
    50,
    0x12476A,
    0xFFFFFF,
    ACT_START_LEARN
  );

  add_gesture_handler();
}

static void render_learning(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x000000);

  add_header(
    "LEARNING ENVIRONMENT",
    "Wind, waves and rod movement",
    0xF5F7FA,
    0x38BDF8,
    true
  );

  add_chart_panel(true);

  big_label = lv_label_create(app_screen);


  set_label_font(big_label, FONT_BIG);
  lv_label_set_text(big_label, "0:20");
  lv_obj_set_style_text_color(big_label, lv_color_hex(0xF5F7FA), 0);
  lv_obj_align(big_label, LV_ALIGN_BOTTOM_MID, 0, -69);

  add_progress(100);

  lv_obj_align(progress_bar, LV_ALIGN_BOTTOM_MID, 0, -45);

  add_gesture_handler();
}

static void render_learn_complete(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x03160B);

  add_header(
    "LEARNING COMPLETE",
    "Environment profile saved",
    0xFFFFFF,
    0x4ADE80,
    true
  );

  add_center_message(
    "READY",
    "Reel in now and attach your bait.\nWhen finished, press READY TO CAST.",
    0x4ADE80,
    0xC7F7D7
  );

  make_button(
    "READY TO CAST",
    0,
    -38,
    220,
    50,
    0x176B3A,
    0xFFFFFF,
    ACT_READY_TO_CAST
  );

  add_gesture_handler();
}

static void render_cast_ready(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x000000);

  add_header(
    "READY TO CAST",
    "Bait attached and rod ready",
    0xF5F7FA,
    0x8C96A3,
    true
  );

  add_center_message(
    "CAST MODE",
    "Press below when you are ready to begin the 50 second cast window.",
    0x38BDF8,
    0xAAB3BE
  );

  make_button(
    "START CAST",
    0,
    -38,
    210,
    50,
    0x12476A,
    0xFFFFFF,
    ACT_READY_TO_CAST
  );

  add_gesture_handler();
}

static void render_cast_countdown(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x071019);

  add_header(
    "CAST MODE",
    "Cast, place rod and tighten line",
    0xFFFFFF,
    0x38BDF8,
    true
  );

  big_label = lv_label_create(app_screen);


  set_label_font(big_label, FONT_BIG);
  lv_label_set_text(big_label, "50");
  lv_obj_set_style_text_color(big_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(big_label, LV_ALIGN_CENTER, 0, -15);

  detail_label = lv_label_create(app_screen);


  set_label_font(detail_label, FONT_UI);
  lv_label_set_text(detail_label, "seconds remaining");
  lv_obj_set_style_text_color(detail_label, lv_color_hex(0x8C96A3), 0);
  lv_obj_align(detail_label, LV_ALIGN_CENTER, 0, 18);

  add_progress(100);

  make_button(
    "READY NOW",
    0,
    -38,
    190,
    50,
    0x176B3A,
    0xFFFFFF,
    ACT_READY_NOW
  );

  add_gesture_handler();
}

static void render_settling(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x071019);

  add_header(
    "SETTING BASELINE",
    "Keep the rod still",
    0xFFFFFF,
    0x38BDF8,
    true
  );

  add_center_message(
    "3",
    "Measuring the resting bend after this cast.",
    0xFFFFFF,
    0xAAB3BE
  );

  add_gesture_handler();
}


static void format_strike_ago(char *buf, size_t len)
{
  if (!strike_history_valid) {
    snprintf(buf, len, "No saved strike");
    return;
  }

  uint32_t sec = (millis() - last_strike_ms) / 1000UL;

  if (sec < 60UL)
    snprintf(buf, len, "%lu sec ago", (unsigned long)sec);
  else
    snprintf(buf, len, "%lu min ago", (unsigned long)(sec / 60UL));
}

static void strike_history_add(int value)
{
  if (strike_history_count < STRIKE_HISTORY_SAMPLES)
    strike_history[strike_history_count++] = value;
}

static void start_strike_recording(void)
{
  strike_history_count = 0;
  strike_history_valid = true;
  strike_event_complete = false;
  strike_peak_count = 3;
  strike_peak_delta = 0;
  strike_duration_ms = 0;
  strike_event_started_ms = millis();
  last_strike_ms = millis();
  strike_sim_step = 0;

  // Save a little pre-strike baseline context.
  for (int i = 0; i < 18; i++) {
    int wobble = (i % 5 == 0) ? 4 : ((i % 3 == 0) ? -3 : 0);
    strike_history_add(baseline_value + wobble);
  }
}

static void render_strike_history(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x000000);
  add_watch_battery_only();

  title_label = lv_label_create(app_screen);
  set_label_font(title_label, FONT_TITLE);
  lv_label_set_text(title_label, "LAST STRIKE");
  lv_obj_set_style_text_color(title_label, lv_color_hex(0xF5F7FA), 0);
  lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 76);

  char ago[32];
  format_strike_ago(ago, sizeof(ago));

  subtitle_label = lv_label_create(app_screen);
  set_label_font(subtitle_label, FONT_UI);
  lv_label_set_text(subtitle_label, ago);
  lv_obj_set_style_text_color(subtitle_label, lv_color_hex(0xFFB020), 0);
  lv_obj_align(subtitle_label, LV_ALIGN_TOP_MID, 0, 105);

  lv_obj_t *panel = make_panel(app_screen, 350, 196, 22, 0x0D1117, 0x252B35, 24);
  lv_obj_add_event_cb(panel, app_gesture_event, LV_EVENT_GESTURE, NULL);

  lv_obj_t *chart = lv_chart_create(panel);
  lv_obj_add_event_cb(chart, app_gesture_event, LV_EVENT_GESTURE, NULL);
  lv_obj_set_size(chart, 316, 112);
  lv_obj_align(chart, LV_ALIGN_TOP_MID, 0, 12);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, 0, 0);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, STRIKE_HISTORY_SAMPLES);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, baseline_value - 80, baseline_value + 260);
  lv_chart_set_div_line_count(chart, 3, 6);

  lv_chart_series_t *series =
    lv_chart_add_series(chart, lv_color_hex(0xFF4D4F), LV_CHART_AXIS_PRIMARY_Y);

  for (int i = 0; i < STRIKE_HISTORY_SAMPLES; i++) {
    int v = (i < strike_history_count) ? strike_history[i] : baseline_value;
    lv_chart_set_next_value(chart, series, v);
  }

  char details[96];
  if (strike_history_valid) {
    snprintf(details, sizeof(details),
             "%d PEAKS   +%d peak   %.1f sec",
             strike_peak_count,
             strike_peak_delta,
             strike_duration_ms / 1000.0f);
  } else {
    snprintf(details, sizeof(details), "No strike recorded yet");
  }

  lv_obj_t *info = lv_label_create(panel);
  set_label_font(info, FONT_UI);
  lv_label_set_text(info, details);
  lv_obj_set_style_text_color(info, lv_color_hex(0xD6DBE1), 0);
  lv_obj_align(info, LV_ALIGN_BOTTOM_MID, 0, -18);

  make_button("BACK TO LIVE", 0, -38, 170, 46, 0x173A52, 0xFFFFFF, ACT_HISTORY_BACK);

  lv_obj_t *hint = lv_label_create(app_screen);
  set_label_font(hint, FONT_UI);
  lv_label_set_text(hint, "Swipe vertically or tap BACK");
  lv_obj_set_style_text_color(hint, lv_color_hex(0x7E8795), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

  add_gesture_handler();
}

static void render_monitoring(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x000000);

  add_watch_battery_only();

  title_label = lv_label_create(app_screen);
  set_label_font(title_label, FONT_TITLE);
  lv_label_set_text(title_label, "MONITORING");
  lv_obj_set_style_text_color(title_label, lv_color_hex(0xF5F7FA), 0);
  lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 73);

  subtitle_label = lv_label_create(app_screen);
  set_label_font(subtitle_label, FONT_UI);
  lv_label_set_text(subtitle_label, "ROD 1 ACTIVE");
  lv_obj_set_style_text_color(subtitle_label, lv_color_hex(0x4ADE80), 0);
  lv_obj_align(subtitle_label, LV_ALIGN_TOP_MID, 0, 101);

  lv_obj_t *panel = make_panel(app_screen, 352, 224, 28, 0x0D1117, 0x253040, 24);

  lv_obj_t *rodinfo = lv_label_create(panel);
  set_label_font(rodinfo, FONT_TITLE);
  lv_label_set_text(rodinfo, "ROD 1   92%   GOOD");
  lv_obj_set_style_text_color(rodinfo, lv_color_hex(0x38BDF8), 0);
  lv_obj_align(rodinfo, LV_ALIGN_TOP_LEFT, 16, 12);

  static const int bh[4] = {7, 11, 15, 19};
  for (int i = 0; i < 4; i++) {
    lv_obj_t *bar = lv_obj_create(panel);
    lv_obj_set_size(bar, 4, bh[i]);
    lv_obj_set_pos(bar, 250 + i * 7, 12 + (19 - bh[i]));
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x38BDF8), 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  }

  value_label = lv_label_create(panel);
  set_label_font(value_label, FONT_UI);
  char vbuf[20];
  snprintf(vbuf, sizeof(vbuf), "%d", current_value);
  lv_label_set_text(value_label, vbuf);
  lv_obj_set_style_text_color(value_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -14, 45);

  main_chart = lv_chart_create(panel);
  lv_obj_set_size(main_chart, 316, 144);
  lv_obj_align(main_chart, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_set_style_bg_opa(main_chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(main_chart, 0, 0);
  lv_obj_set_style_pad_all(main_chart, 0, 0);
  lv_chart_set_type(main_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(main_chart, 70);
  lv_chart_set_range(main_chart, LV_CHART_AXIS_PRIMARY_Y, baseline_value - 100, baseline_value + 280);
  lv_chart_set_div_line_count(main_chart, 4, 6);
  main_series = lv_chart_add_series(main_chart, lv_color_hex(0x38BDF8), LV_CHART_AXIS_PRIMARY_Y);
  for (int i = 0; i < 70; i++) lv_chart_set_next_value(main_chart, main_series, baseline_value);

  make_button("ADJUST ROD", -78, -54, 142, 44, 0x303742, 0xFFFFFF, ACT_ADJUST_ROD);
  make_button("SIM STRIKE",  78, -54, 142, 44, 0x6B1C1C, 0xFFFFFF, ACT_SIM_STRIKE);

  lv_obj_t *hint = lv_label_create(app_screen);
  set_label_font(hint, FONT_UI);
  lv_label_set_text(hint, strike_history_valid ? "Vertical swipe: last strike" : "Vertical swipe: strike history");
  lv_obj_set_style_text_color(hint, lv_color_hex(0x606A78), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);

  add_gesture_handler();
}

static void render_strike(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x260606);

  add_watch_battery_only();

  title_label = lv_label_create(app_screen);
  set_label_font(title_label, FONT_BIG);
  lv_label_set_text(title_label, "STRIKE!");
  lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 64);

  subtitle_label = lv_label_create(app_screen);
  set_label_font(subtitle_label, FONT_UI);
  lv_label_set_text(subtitle_label, "ROD 1  •  CONFIRM ACTION");
  lv_obj_set_style_text_color(subtitle_label, lv_color_hex(0xFFB7B7), 0);
  lv_obj_align(subtitle_label, LV_ALIGN_TOP_MID, 0, 106);

  lv_obj_t *panel = make_panel(app_screen, 352, 190, 23, 0x120303, 0xA92D2D, 24);

  main_chart = lv_chart_create(panel);
  lv_obj_set_size(main_chart, 316, 118);
  lv_obj_align(main_chart, LV_ALIGN_TOP_MID, 0, 12);
  lv_obj_set_style_bg_opa(main_chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(main_chart, 0, 0);
  lv_obj_set_style_pad_all(main_chart, 0, 0);
  lv_chart_set_type(main_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(main_chart, STRIKE_HISTORY_SAMPLES);
  lv_chart_set_range(main_chart, LV_CHART_AXIS_PRIMARY_Y, baseline_value - 80, baseline_value + 260);
  lv_chart_set_div_line_count(main_chart, 3, 6);
  main_series = lv_chart_add_series(main_chart, lv_color_hex(0xFFFFFF), LV_CHART_AXIS_PRIMARY_Y);

  for (int i = 0; i < STRIKE_HISTORY_SAMPLES; i++) {
    int v = (i < strike_history_count) ? strike_history[i] : baseline_value;
    lv_chart_set_next_value(main_chart, main_series, v);
  }

  detail_label = lv_label_create(panel);
  set_label_font(detail_label, FONT_UI);
  lv_label_set_text(detail_label,
                    strike_event_complete ? "3 peaks recorded • event saved"
                                          : "Recording strike pattern...");
  lv_obj_set_style_text_color(detail_label, lv_color_hex(0xFFD2D2), 0);
  lv_obj_align(detail_label, LV_ALIGN_BOTTOM_MID, 0, -14);

  make_button("I'M WATCHING", -78, -52, 145, 46, 0x7A3415, 0xFFFFFF, ACT_WATCHING);
  make_button("REEL IN",       78, -52, 145, 46, 0x7D1F1F, 0xFFFFFF, ACT_REEL_IN);

  add_gesture_handler();
}

static void render_reel_paused(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x211700);

  add_header(
    "REEL IN",
    "DETECTION PAUSED",
    0xFFF4D6,
    0xF9C74F,
    true
  );

  add_center_message(
    "PAUSED",
    "Move the rod, reel in, land the fish or rebait.\nNo bite alarms will trigger.",
    0xF9C74F,
    0xD6C58F
  );

  make_button(
    "READY TO CAST",
    0,
    -38,
    220,
    50,
    0x805E0A,
    0xFFFFFF,
    ACT_READY_TO_CAST
  );

  add_gesture_handler();
}

static void render_adjust(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x211700);

  add_header(
    "ADJUST ROD",
    "DETECTION PAUSED",
    0xFFF4D6,
    0xF9C74F,
    true
  );

  add_center_message(
    "ADJUST NOW",
    "Tighten the line, move the rod or reposition the rest.",
    0xF9C74F,
    0xD6C58F
  );

  make_button(
    "READY",
    0,
    -38,
    180,
    50,
    0x805E0A,
    0xFFFFFF,
    ACT_ADJUST_READY
  );

  add_gesture_handler();
}

static void update_signal_visuals(void)
{
  uint32_t active_color = 0x4ADE80;
  const char *quality = "GOOD";
  const char *rssi = "RSSI  -65 dBm";

  if (signal_level <= 1) {
    active_color = 0xEF4444;
    quality = "POOR";
    rssi = "RSSI  -115 dBm";
  }
  else if (signal_level == 2) {
    active_color = 0xF59E0B;
    quality = "WEAK";
    rssi = "RSSI  -100 dBm";
  }
  else if (signal_level == 3) {
    active_color = 0xEAB308;
    quality = "FAIR";
    rssi = "RSSI  -82 dBm";
  }

  for (int i = 0; i < 4; i++) {
    lv_obj_set_style_bg_color(
      signal_bars[i],
      lv_color_hex(i < signal_level ? active_color : 0x252B35),
      0
    );
  }

  if (subtitle_label != NULL) {
    lv_label_set_text(subtitle_label, quality);
    lv_obj_set_style_text_color(subtitle_label, lv_color_hex(active_color), 0);
  }

  if (detail_label != NULL) {
    lv_label_set_text(detail_label, rssi);
  }
}

static void app_render_signal_page(void)
{
  lv_obj_clean(app_screen);
  set_screen_bg(0x000000);

  add_connected_batteries();

  title_label = lv_label_create(app_screen);


  set_label_font(title_label, FONT_TITLE);
  lv_label_set_text(title_label, "ROD LINK");
  lv_obj_set_style_text_color(title_label, lv_color_hex(0xF5F7FA), 0);
  lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 82);

  subtitle_label = lv_label_create(app_screen);


  set_label_font(subtitle_label, FONT_UI);
  lv_label_set_text(subtitle_label, "GOOD");
  lv_obj_set_style_text_color(subtitle_label, lv_color_hex(0x4ADE80), 0);
  lv_obj_align(subtitle_label, LV_ALIGN_TOP_MID, 0, 108);

  lv_obj_t *panel = make_panel(
    app_screen,
    340,
    205,
    8,
    0x0D1117,
    0x252B35,
    24
  );

  lv_obj_t *rod_text = lv_label_create(panel);
set_label_font(rod_text, FONT_UI);
  lv_label_set_text(rod_text, "ROD");
  lv_obj_set_style_text_color(rod_text, lv_color_hex(0xAAB3BE), 0);
  lv_obj_align(rod_text, LV_ALIGN_LEFT_MID, 25, 0);

  lv_obj_t *watch_text = lv_label_create(panel);
set_label_font(watch_text, FONT_UI);
  lv_label_set_text(watch_text, "WATCH");
  lv_obj_set_style_text_color(watch_text, lv_color_hex(0xAAB3BE), 0);
  lv_obj_align(watch_text, LV_ALIGN_RIGHT_MID, -22, 0);

  const int heights[4] = {30, 48, 66, 84};
  const int xpos[4] = {-60, -20, 20, 60};

  for (int i = 0; i < 4; i++) {
    signal_bars[i] = lv_obj_create(panel);
    lv_obj_set_size(signal_bars[i], 25, heights[i]);
    lv_obj_align(signal_bars[i], LV_ALIGN_CENTER, xpos[i], -5);
    lv_obj_set_style_radius(signal_bars[i], 7, 0);
    lv_obj_set_style_border_width(signal_bars[i], 0, 0);
    lv_obj_set_style_pad_all(signal_bars[i], 0, 0);
    lv_obj_clear_flag(signal_bars[i], LV_OBJ_FLAG_SCROLLABLE);
  }

  detail_label = lv_label_create(panel);


  set_label_font(detail_label, FONT_UI);
  lv_label_set_text(detail_label, "RSSI  -65 dBm");
  lv_obj_set_style_text_color(detail_label, lv_color_hex(0x8C96A3), 0);
  lv_obj_align(detail_label, LV_ALIGN_BOTTOM_MID, 0, -18);

  lv_obj_t *hint = lv_label_create(app_screen);
set_label_font(hint, FONT_UI);
  lv_label_set_text(hint, "SWIPE RIGHT TO RETURN");
  lv_obj_set_style_text_color(hint, lv_color_hex(0x5F6875), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -37);

  lv_obj_add_event_cb(
    app_screen,
    app_gesture_event,
    LV_EVENT_GESTURE,
    NULL
  );

  update_signal_visuals();
}

static void app_render_current(void)
{
  signal_page_visible = false;

  title_label = NULL;
  subtitle_label = NULL;
  big_label = NULL;
  detail_label = NULL;
  main_chart = NULL;
  main_series = NULL;
  value_label = NULL;
  base_label = NULL;
  change_label = NULL;
  progress_bar = NULL;

  switch (app_state)
  {
    case APP_BOOT:
      render_boot();
      break;

    case APP_CONNECTING:
      render_connecting();
      break;

    case APP_CONNECT_FAILED:
      render_connect_failed();
      break;

    case APP_CONNECTED:
      render_connected();
      break;

    case APP_LEARN_PROMPT:
      render_learn_prompt();
      break;

    case APP_LEARNING:
      render_learning();
      break;

    case APP_LEARN_COMPLETE:
      render_learn_complete();
      break;

    case APP_CAST_READY:
      render_cast_ready();
      break;

    case APP_CAST_COUNTDOWN:
      render_cast_countdown();
      break;

    case APP_SETTLING:
      render_settling();
      break;

    case APP_MONITORING:
      render_monitoring();
      break;

    case APP_STRIKE:
      render_strike();
      break;

    case APP_REEL_PAUSED:
      render_reel_paused();
      break;

    case APP_ADJUST:
      render_adjust();
      break;
  }
}

static void app_set_state(app_state_t new_state)
{
  history_page_visible = false;
  app_state = new_state;
  state_started_ms = app_now_ms();

  if (new_state != APP_STRIKE) {
    strike_running = false;
    strike_step = 0;
  }

  app_render_current();
}

static void app_button_event(lv_event_t *e)
{
  app_action_t action =
    (app_action_t)(intptr_t)lv_event_get_user_data(e);

  switch (action)
  {
    case ACT_RETRY_CONNECT:
      rod_connected = false;
      app_set_state(APP_CONNECTING);
      break;

    case ACT_START_LEARN:
      app_set_state(APP_LEARNING);
      break;

    case ACT_READY_TO_CAST:
      app_set_state(APP_CAST_COUNTDOWN);
      break;

    case ACT_READY_NOW:
      app_set_state(APP_SETTLING);
      break;

    case ACT_SIM_STRIKE:
      strike_running = true;
      strike_step = 0;
      start_strike_recording();
      app_set_state(APP_STRIKE);
      break;

    case ACT_WATCHING:
      app_set_state(APP_MONITORING);
      break;

    case ACT_REEL_IN:
      app_set_state(APP_REEL_PAUSED);
      break;

    case ACT_HISTORY_BACK:
      history_page_visible = false;
      app_set_state(APP_MONITORING);
      break;

    case ACT_ADJUST_ROD:
      app_set_state(APP_ADJUST);
      break;

    case ACT_ADJUST_READY:
      app_set_state(APP_SETTLING);
      break;

    default:
      break;
  }
}

static void app_gesture_event(lv_event_t *e)
{
  (void)e;

  if (!rod_connected) {
    return;
  }

  lv_indev_t *indev = lv_indev_get_act();

  if (indev == NULL) {
    return;
  }

  lv_dir_t dir = lv_indev_get_gesture_dir(indev);

  // The board is rotated, so LVGL's TOP/BOTTOM direction can feel reversed.
  // Accept either vertical swipe: LIVE -> HISTORY, HISTORY -> LIVE.
  if (dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM) {
    if (history_page_visible) {
      history_page_visible = false;
      app_set_state(APP_MONITORING);
    }
    else if (app_state == APP_MONITORING) {
      history_page_visible = true;
      render_strike_history();
    }
    return;
  }

  if (!signal_page_visible && dir == LV_DIR_LEFT) {
    signal_return_state = app_state;
    signal_page_visible = true;
    app_render_signal_page();
  }
  else if (signal_page_visible && dir == LV_DIR_RIGHT) {
    signal_page_visible = false;
    app_state = signal_return_state;
    app_render_current();
  }
}

static void update_graph_sample(bool learning)
{
  static const int calm_pattern[] =
  {
     0,  3,  1, -2,
     0,  5,  8,  3,
    -2, -5, -1,  4,
     2,  0, -3,  1
  };

  // Three clear fish pulls followed by release back to baseline.
  static const int realistic_strike[] =
  {
     0,  2,  1,  0,
     6, 18, 42, 78,118, 76, 34, 10,  2,
     4, 16, 38, 70,103, 68, 28,  8,  1,
     3, 14, 34, 62, 91, 55, 21,  6,  0,
    -3, -7, -4, -2,  0,  1,  0, -1,  0,
     0,  0,  1,  0,  0,  0
  };

  int variation =
    calm_pattern[
      calm_step %
      (sizeof(calm_pattern) / sizeof(calm_pattern[0]))
    ];

  calm_step++;

  if (learning)
    variation *= 4;

  current_value = baseline_value + variation;

  if (strike_running && strike_sim_step >= 0) {
    const int scount = sizeof(realistic_strike) / sizeof(realistic_strike[0]);

    if (strike_sim_step < scount) {
      int delta = realistic_strike[strike_sim_step];
      current_value = baseline_value + delta;

      if (delta > strike_peak_delta)
        strike_peak_delta = delta;

      strike_history_add(current_value);
      strike_sim_step++;
    }

    if (strike_sim_step >= scount) {
      strike_event_complete = true;
      strike_duration_ms = millis() - strike_event_started_ms;
      strike_sim_step = -1;
      strike_running = false;
      current_value = baseline_value;

      if (detail_label != NULL && app_state == APP_STRIKE)
        lv_label_set_text(detail_label, "3 peaks recorded • event saved");
    }
  }

  if (main_chart != NULL && main_series != NULL) {
    lv_chart_set_next_value(main_chart, main_series, current_value);
  }

  update_monitor_values();
}

static void app_timer_cb(lv_timer_t *timer)
{
  (void)timer;

  if (history_page_visible) return;

  uint32_t elapsed = app_elapsed_ms();
  uint32_t now = app_now_ms();

  // The signal screen is an overlay. Timers underneath continue to run.
  if (signal_page_visible) {
    if ((now - last_signal_ms) >= 1200UL) {
      static const uint8_t levels[] = {4, 4, 3, 3, 2, 2, 1, 1, 2, 3, 4};
      int count = sizeof(levels) / sizeof(levels[0]);

      signal_level = levels[signal_demo_step % count];
      signal_demo_step++;

      update_signal_visuals();
      last_signal_ms = now;
    }
  }

  switch (app_state)
  {
    case APP_BOOT:
      if (elapsed >= BOOT_DURATION_MS) {
        app_set_state(APP_CONNECTING);
      }
      break;

    case APP_CONNECTING:
#if DEMO_AUTO_CONNECT
      if (elapsed >= CONNECT_DURATION_MS) {
        rod_connected = true;
        app_set_state(APP_CONNECTED);
      }
#else
      if (elapsed >= CONNECT_FAIL_MS) {
        rod_connected = false;
        app_set_state(APP_CONNECT_FAILED);
      }
#endif
      break;

    case APP_CONNECTED:
      if (elapsed >= CONNECTED_HOLD_MS) {
        if (session_learned) {
          app_set_state(APP_CAST_READY);
        }
        else {
          app_set_state(APP_LEARN_PROMPT);
        }
      }
      break;

    case APP_LEARNING:
    {
      uint32_t remaining_ms =
        (elapsed >= LEARN_DURATION_MS) ?
        0 :
        (LEARN_DURATION_MS - elapsed);

      uint32_t remaining_s = (remaining_ms + 999UL) / 1000UL;
      uint32_t minutes = remaining_s / 60UL;
      uint32_t seconds = remaining_s % 60UL;

      if (!signal_page_visible) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu:%02lu",
                 (unsigned long)minutes,
                 (unsigned long)seconds);

        if (big_label != NULL) {
          lv_label_set_text(big_label, buf);
        }

        if (progress_bar != NULL) {
          int percent =
            (int)((elapsed * 100UL) / LEARN_DURATION_MS);

          if (percent > 100) percent = 100;

          lv_bar_set_value(progress_bar, percent, LV_ANIM_OFF);
        }

        if ((now - last_graph_ms) >= 180UL) {
          update_graph_sample(true);
          last_graph_ms = now;
        }
      }

      if (elapsed >= LEARN_DURATION_MS) {
        session_learned = true;
        app_set_state(APP_LEARN_COMPLETE);
      }
      break;
    }

    case APP_CAST_COUNTDOWN:
    {
      uint32_t remaining_ms =
        (elapsed >= CAST_DURATION_MS) ?
        0 :
        (CAST_DURATION_MS - elapsed);

      uint32_t remaining_s = (remaining_ms + 999UL) / 1000UL;

      if (!signal_page_visible) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)remaining_s);

        if (big_label != NULL) {
          lv_label_set_text(big_label, buf);
        }

        if (progress_bar != NULL) {
          int percent =
            (int)((elapsed * 100UL) / CAST_DURATION_MS);

          if (percent > 100) percent = 100;

          lv_bar_set_value(progress_bar, percent, LV_ANIM_OFF);
        }
      }

      if (elapsed >= CAST_DURATION_MS) {
        app_set_state(APP_SETTLING);
      }
      break;
    }

    case APP_SETTLING:
    {
      uint32_t remaining_ms =
        (elapsed >= SETTLE_DURATION_MS) ?
        0 :
        (SETTLE_DURATION_MS - elapsed);

      uint32_t remaining_s = (remaining_ms + 999UL) / 1000UL;

      if (!signal_page_visible && big_label != NULL) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)remaining_s);
        lv_label_set_text(big_label, buf);
      }

      if (elapsed >= SETTLE_DURATION_MS) {
        // In the real product this would be the average flex-sensor reading.
        baseline_value = current_value;
        app_set_state(APP_MONITORING);
      }
      break;
    }

    case APP_MONITORING:
      if (!signal_page_visible &&
          (now - last_graph_ms) >= 100UL) {
        update_graph_sample(false);
        last_graph_ms = now;
      }
      break;

    case APP_STRIKE:
      if (!signal_page_visible &&
          (now - last_graph_ms) >= 100UL) {
        update_graph_sample(false);
        last_graph_ms = now;
      }
      break;

    default:
      break;
  }
}

static void fish_create_ui(void)
{
  app_screen = lv_scr_act();

  lv_obj_clear_flag(app_screen, LV_OBJ_FLAG_SCROLLABLE);

  app_state = APP_BOOT;
  rod_connected = false;
  session_learned = false;
  signal_page_visible = false;
  strike_running = false;
  history_page_visible = false;
  strike_history_valid = false;
  strike_history_count = 0;
  strike_event_complete = false;
  strike_sim_step = -1;

  baseline_value = 2000;
  current_value = 2000;

  state_started_ms = app_now_ms();
  last_graph_ms = state_started_ms;
  last_signal_ms = state_started_ms;

  app_render_current();

  lv_timer_create(
    app_timer_cb,
    100,
    NULL
  );
}


static const sh8601_lcd_init_cmd_t sh8601_lcd_init_cmds[] =
{
  {0x11, (uint8_t []){0x00}, 0, 120},
  {0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
  {0x35, (uint8_t []){0x00}, 1, 0},
  {0x53, (uint8_t []){0x20}, 1, 10},
  {0x51, (uint8_t []){0x00}, 1, 10},
  {0x29, (uint8_t []){0x00}, 0, 10},
  {0x51, (uint8_t []){0xFF}, 1, 0},
  //{0x36, (uint8_t []){0x80}, 1, 0},
};
static const sh8601_lcd_init_cmd_t co5300_lcd_init_cmds[] =
{
  {0x11, (uint8_t []){0x00}, 0, 80},
  {0xC4, (uint8_t []){0x80}, 1, 0},
  //{0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
  //{0x35, (uint8_t []){0x00}, 1, 0},//TE ON
  {0x53, (uint8_t []){0x20}, 1, 1},
  {0x63, (uint8_t []){0xFF}, 1, 1},
  {0x51, (uint8_t []){0x00}, 1, 1},
  {0x29, (uint8_t []){0x00}, 0, 10},
  {0x51, (uint8_t []){0xFF}, 1, 0},
  //{0x36, (uint8_t []){0x60}, 1, 0},
};


void lcd_lvgl_Init(void)
{
  READ_LCD_ID = read_lcd_id();
  static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
  static lv_disp_drv_t disp_drv;      // contains callback functions

  const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_PCLK,
                                                               EXAMPLE_PIN_NUM_LCD_DATA0,
                                                               EXAMPLE_PIN_NUM_LCD_DATA1,
                                                               EXAMPLE_PIN_NUM_LCD_DATA2,
                                                               EXAMPLE_PIN_NUM_LCD_DATA3,
                                                               EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * LCD_BIT_PER_PIXEL / 8);
  ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
  esp_lcd_panel_io_handle_t io_handle = NULL;

  const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_CS,
                                                                              example_notify_lvgl_flush_ready,
                                                                              &disp_drv);

  sh8601_vendor_config_t vendor_config =
  {
    .flags =
    {
      .use_qspi_interface = 1,
    },
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
  esp_lcd_panel_handle_t panel_handle = NULL;
  const esp_lcd_panel_dev_config_t panel_config =
  {
    .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = LCD_BIT_PER_PIXEL,
    .vendor_config = &vendor_config,
  };
  vendor_config.init_cmds = (READ_LCD_ID == SH8601_ID) ? sh8601_lcd_init_cmds : co5300_lcd_init_cmds;
  vendor_config.init_cmds_size = (READ_LCD_ID == SH8601_ID) ? sizeof(sh8601_lcd_init_cmds) / sizeof(sh8601_lcd_init_cmds[0]) : sizeof(co5300_lcd_init_cmds) / sizeof(co5300_lcd_init_cmds[0]);
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_init(panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(panel_handle, true));

  lv_init();
  lv_color_t *buf1 = heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
  assert(buf1);
  lv_color_t *buf2 = heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
  assert(buf2);
  lv_disp_draw_buf_init(&disp_buf, buf1, buf2, EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT);
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = EXAMPLE_LCD_H_RES;
  disp_drv.ver_res = EXAMPLE_LCD_V_RES;
  disp_drv.flush_cb = example_lvgl_flush_cb;
  disp_drv.rounder_cb = example_lvgl_rounder_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.user_data = panel_handle;
#ifdef EXAMPLE_Rotate_90
  disp_drv.sw_rotate = 1;
  disp_drv.rotated = LV_DISP_ROT_270;
#endif
  lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;    // Input device driver (Touch)
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.disp = disp;
  indev_drv.read_cb = example_lvgl_touch_cb;
  lv_indev_drv_register(&indev_drv);

  const esp_timer_create_args_t lvgl_tick_timer_args =
  {
    .callback = &example_increase_lvgl_tick,
    .name = "lvgl_tick"
  };
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

  lvgl_mux = xSemaphoreCreateMutex(); //mutex semaphores
  assert(lvgl_mux);
  xTaskCreate(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);
  if (example_lvgl_lock(-1))
  {
    fish_create_ui();

    // Release the mutex
    example_lvgl_unlock();
  }
}

static bool example_lvgl_lock(int timeout_ms)
{
  assert(lvgl_mux && "bsp_display_start must be called first");

  const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

static void example_lvgl_unlock(void)
{
  assert(lvgl_mux && "bsp_display_start must be called first");
  xSemaphoreGive(lvgl_mux);
}
static void example_lvgl_port_task(void *arg)
{
  uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
  for(;;)
  {
    if (example_lvgl_lock(-1))
    {
      task_delay_ms = lv_timer_handler();

      example_lvgl_unlock();
    }
    if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS)
    {
      task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    }
    else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS)
    {
      task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}
static void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
  lv_disp_flush_ready(disp_driver);
  return false;
}
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
  const int offsetx1 = (READ_LCD_ID == SH8601_ID) ? area->x1 : area->x1 + 0x06;
  const int offsetx2 = (READ_LCD_ID == SH8601_ID) ? area->x2 : area->x2 + 0x06;
  const int offsety1 = area->y1;
  const int offsety2 = area->y2;

  esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}
void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
  uint16_t x1 = area->x1;
  uint16_t x2 = area->x2;

  uint16_t y1 = area->y1;
  uint16_t y2 = area->y2;

  // round the start of coordinate down to the nearest 2M number
  area->x1 = (x1 >> 1) << 1;
  area->y1 = (y1 >> 1) << 1;
  // round the end of coordinate up to the nearest 2N+1 number
  area->x2 = ((x2 >> 1) << 1) + 1;
  area->y2 = ((y2 >> 1) << 1) + 1;
}
static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  uint16_t tp_x,tp_y;
  uint8_t win = getTouch(&tp_x,&tp_y);
  if(win)
  {
    data->point.x = tp_x;
    data->point.y = tp_y;
    data->state = LV_INDEV_STATE_PRESSED;
  }
  else
  {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

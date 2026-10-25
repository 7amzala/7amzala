// =====================================================
// PAIR YOUR ROD (watch, first start or after "forget")
//
// Shown instead of the normal screens while the watch has no rod:
// the rods open for pairing nearby (switched on in the last 3 min) by
// their ID, pick yours, type its PIN (on the rod box), done. Then the
// watch restarts into the normal screens and only ever talks to that rod.
// =====================================================

#include "lvgl.h"
#include "ble_link.h"
#include "pair_ui.h"
#include <stdio.h>
#include <string.h>

void pair_restart(void);           // StrikeSense_Watch.ino: restart into the normal screens

#define COL_BG    lv_color_hex(0x000000)
#define COL_CYAN  lv_color_hex(0x22d3ff)
#define COL_TEXT  lv_color_hex(0xeaf3ff)
#define COL_DIM   lv_color_hex(0x8ea6c8)
#define COL_CARD  lv_color_hex(0x141924)
#define COL_GREEN lv_color_hex(0x3ddc84)
#define COL_RED   lv_color_hex(0xff4d5e)

#define ROWS 4

typedef enum { P_LIST, P_PIN, P_WORKING, P_OK, P_FAILED } page_t;

static page_t page = P_LIST;
static lv_obj_t *head, *info, *pin_label;
static lv_obj_t *rod_btn[ROWS], *rod_name[ROWS], *rod_sig[ROWS];
static lv_obj_t *keys[12];
static lv_obj_t *retry;
static char rod_ids[ROWS][12];
static char chosen[12];
static char digits[7];
static int ndigits = 0;
static uint32_t page_ms = 0;

static void set_text(lv_obj_t *o, const char *t)
{
  if(strcmp(lv_label_get_text(o), t) != 0)
    lv_label_set_text(o, t);
}

static void show(lv_obj_t *o, bool on)
{
  if(on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *text(const lv_font_t *f, lv_color_t c)
{
  lv_obj_t *l = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, "");
  return l;
}

static lv_obj_t *button(int w, int h)
{
  lv_obj_t *b = lv_btn_create(lv_scr_act());
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, COL_CARD, 0);
  lv_obj_set_style_radius(b, h / 2, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  return b;
}

static void layout(void)
{
  for(int i = 0; i < ROWS; i++) show(rod_btn[i], false);
  for(int i = 0; i < 12; i++) show(keys[i], page == P_PIN);
  show(pin_label, page == P_PIN);
  show(retry, page == P_FAILED);
  page_ms = lv_tick_get();
}

static void pin_text(void)
{
  char t[20];
  int p = 0;

  for(int i = 0; i < 6; i++)
  {
    t[p++] = i < ndigits ? digits[i] : '_';
    if(i == 2) t[p++] = ' ';
  }

  t[p] = 0;
  set_text(pin_label, t);
}

static void go(page_t p)
{
  page = p;
  layout();

  switch(p)
  {
    case P_LIST:
      set_text(head, "PAIR YOUR ROD");
      lv_obj_set_style_text_color(head, COL_DIM, 0);
      break;

    case P_PIN:
      set_text(head, chosen);
      lv_obj_set_style_text_color(head, COL_CYAN, 0);
      set_text(info, "type the PIN from the rod box");
      ndigits = 0;
      pin_text();
      break;

    case P_WORKING:
      set_text(info, "Checking the PIN...");
      break;

    case P_OK:
      set_text(head, chosen);
      lv_obj_set_style_text_color(head, COL_GREEN, 0);
      set_text(info, LV_SYMBOL_OK " Paired\nonly this rod, encrypted");
      break;

    case P_FAILED:
      set_text(head, "NOT PAIRED");
      lv_obj_set_style_text_color(head, COL_RED, 0);
      set_text(info, "Wrong PIN, or the rod's\n3 minutes are over:\nswitch the rod off and on");
      break;
  }
}

static void rod_clicked(lv_event_t *e)
{
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  strcpy(chosen, rod_ids[i]);
  go(P_PIN);
}

static void key_clicked(lv_event_t *e)
{
  int k = (int)(intptr_t)lv_event_get_user_data(e);    // 0..8 = 1..9, 9 = back, 10 = 0, 11 = OK

  if(k == 9)
  {
    if(ndigits > 0) ndigits--;
    else go(P_LIST);
  }
  else if(k == 11)
  {
    if(ndigits == 6)
    {
      uint32_t pin = 0;
      for(int i = 0; i < 6; i++) pin = pin * 10 + (digits[i] - '0');
      ble_link_pair_start(chosen, pin);
      go(P_WORKING);
      return;
    }
  }
  else if(ndigits < 6)
  {
    digits[ndigits++] = (char)('0' + (k == 10 ? 0 : k + 1));
  }

  if(page == P_PIN)
    pin_text();
}

static void retry_clicked(lv_event_t *e)
{
  (void)e;
  go(P_LIST);
}

static void pair_timer(lv_timer_t *t)
{
  (void)t;

  if(page == P_LIST)
  {
    pair_rod_t r[ROWS];
    int n = ble_link_pair_list(r, ROWS);

    for(int i = 0; i < ROWS; i++)
    {
      show(rod_btn[i], i < n);

      if(i < n)
      {
        strcpy(rod_ids[i], r[i].id);
        set_text(rod_name[i], r[i].id);
        set_text(rod_sig[i], r[i].rssi >= -65 ? "near" : r[i].rssi >= -80 ? "ok" : "far");
      }
    }

    set_text(info, n ? "tap the ID on your rod box" : "Switch your rod on.\nRods show here for 3 min\nafter switching on.");
    lv_obj_align(info, LV_ALIGN_CENTER, 0, n ? 150 : 10);
  }
  else if(page == P_WORKING)
  {
    pair_state_t s = ble_link_pair_state();

    if(s == PAIR_OK) go(P_OK);
    else if(s == PAIR_FAILED || lv_tick_get() - page_ms > 30000) go(P_FAILED);
  }
  else if(page == P_OK && lv_tick_get() - page_ms > 2500)
  {
    static bool once = false;

    if(!once)
    {
      once = true;
      pair_restart();
    }
  }

  if(page != P_LIST)
    lv_obj_align(info, LV_ALIGN_CENTER, 0, page == P_PIN ? -110 : 20);
}

void pair_ui_create(void)
{
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  head = text(&lv_font_montserrat_28, COL_DIM);
  lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 62);

  info = text(&lv_font_montserrat_20, COL_DIM);

  for(int i = 0; i < ROWS; i++)
  {
    rod_btn[i] = button(280, 56);
    lv_obj_align(rod_btn[i], LV_ALIGN_TOP_MID, 0, 120 + i * 64);
    lv_obj_add_event_cb(rod_btn[i], rod_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    rod_name[i] = lv_label_create(rod_btn[i]);
    lv_obj_set_style_text_font(rod_name[i], &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(rod_name[i], COL_TEXT, 0);
    lv_obj_align(rod_name[i], LV_ALIGN_LEFT_MID, 10, 0);

    rod_sig[i] = lv_label_create(rod_btn[i]);
    lv_obj_set_style_text_font(rod_sig[i], &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(rod_sig[i], COL_DIM, 0);
    lv_obj_align(rod_sig[i], LV_ALIGN_RIGHT_MID, -6, 0);
  }

  pin_label = text(&lv_font_montserrat_36, COL_TEXT);
  lv_obj_set_style_text_letter_space(pin_label, 6, 0);
  lv_obj_align(pin_label, LV_ALIGN_TOP_MID, 0, 140);

  static const char *k[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK };

  for(int i = 0; i < 12; i++)
  {
    keys[i] = button(76, 46);
    lv_obj_align(keys[i], LV_ALIGN_TOP_MID, (i % 3 - 1) * 86, 196 + (i / 3) * 54);
    lv_obj_add_event_cb(keys[i], key_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    if(i == 11)
      lv_obj_set_style_bg_color(keys[i], COL_CYAN, 0);

    lv_obj_t *l = lv_label_create(keys[i]);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, i == 11 ? lv_color_hex(0x04121a) : COL_TEXT, 0);
    lv_label_set_text(l, k[i]);
    lv_obj_center(l);
  }

  retry = button(200, 56);
  lv_obj_align(retry, LV_ALIGN_BOTTOM_MID, 0, -80);
  lv_obj_add_event_cb(retry, retry_clicked, LV_EVENT_CLICKED, NULL);
  lv_obj_t *rl = lv_label_create(retry);
  lv_obj_set_style_text_font(rl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(rl, COL_CYAN, 0);
  lv_label_set_text(rl, "TRY AGAIN");
  lv_obj_center(rl);

  go(P_LIST);
  lv_timer_create(pair_timer, 300, NULL);
}

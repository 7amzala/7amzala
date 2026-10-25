// Bluetooth <-> LoRa switching (see link_merge.h)

#include <string.h>
#include <stdlib.h>

#include "link_merge.h"

static link_via_t via = LINK_NONE;
static uint32_t last_hb = 0;          // LoRa heartbeat count already used
static bool have_knock = false;
static uint8_t last_knock8 = 0;       // the rod's knock number, low byte (LoRa)
static long knock_full = 0;           // the full knock number the screens see
static float lora_peak = 0;           // knock peak from LoRa, x the knock limit

static const char *state_name(uint8_t st)
{
  switch(st)
  {
    case ROD_LEARNING: return "learning";
    case ROD_PULLTEST: return "pulltest";
    case ROD_SETTLING: return "settling";
    case ROD_PAUSED:   return "paused";
    default:           return "monitoring";
  }
}

void link_merge(ble_link_status_t *s, const lora_link_status_t *lo, uint32_t now_ms, bool take_peak)
{
  bool bt = s->phase == BLE_LINK_CONNECTED && s->seq > 0 && s->age_ms < LINK_BT_FRESH_MS;
  uint32_t lora_age = lo->last_hb_ms == 0 ? 0xFFFFFFFF : now_ms - lo->last_hb_ms;
  bool lora = lo->hb_count > 0 && lora_age < LINK_LORA_FRESH_MS;
  bool new_hb = lo->hb_count != last_hb;

  s->lora_ok = lo->radio_ok;
  s->lora_bars = lora_link_signal_bars();
  s->lora_chan = lo->chan;
  s->lora_quality = lo->quality;
  s->lora_rods = lo->rods;

  // the knock number: LoRa only sends its low byte, so count on from the last full one
  if(lo->hb_count > 0)
  {
    if(!have_knock)
      last_knock8 = lo->knock_id;

    if(!bt && lora)
      knock_full += (uint8_t)(lo->knock_id - last_knock8);

    last_knock8 = lo->knock_id;
    have_knock = true;
  }

  if(new_hb && !bt && lora && lo->kpeak10 / 10.0f > lora_peak)
    lora_peak = lo->kpeak10 / 10.0f;

  last_hb = lo->hb_count;

  if(bt || !lora)
  {
    // Bluetooth (or nothing: the screens show "looking for the rod")
    via = s->phase == BLE_LINK_CONNECTED && s->seq > 0 ? LINK_BT : LINK_NONE;
    s->via = via;

    if(s->seq > 0)
      knock_full = s->knockid;

    lora_peak = 0;
    return;
  }

  // ---- LoRa ----
  via = LINK_LORA;

  s->phase = BLE_LINK_CONNECTED;
  s->via = LINK_LORA;
  s->rssi = (int)lo->last_rssi;
  s->seq = s->seq + lo->hb_count;      // > 0, grows with each heartbeat
  s->age_ms = lora_age;

  strncpy(s->state, state_name(lo->rod_state), sizeof(s->state) - 1);
  s->state[sizeof(s->state) - 1] = 0;
  s->strike = lo->strike_type != STRIKE_NONE;
  strncpy(s->type, s->strike ? lora_link_strike_name(lo->strike_type) : "", sizeof(s->type) - 1);
  s->type[sizeof(s->type) - 1] = 0;
  s->strikeid = lo->strike_id;
  s->knockid = knock_full;

  // knocks: LoRa sends the peak as x10 of the knock limit
  s->knockthr = 1.0f;
  s->energy_peak = lora_peak;

  if(take_peak)
    lora_peak = 0;

  s->tilt = lo->tilt;
  s->ftilt = lo->ftilt;
  s->allow = lo->allow > 0.01f ? lo->allow : 1.0f;
  s->ptdeg = lo->ptdeg;
  s->ptneed = lo->ptneed > 0.01f ? lo->ptneed : 1.0f;
  s->bucket = lo->bucket;
  s->drop = lo->drop;
  s->knocks = lo->knocks;
  s->wave = lo->wave;
  s->wind = lo->wind;
  s->bat = lo->rod_battery == 255 ? -1 : lo->rod_battery;
  s->batleft = lo->bat_left;
  s->dir = (lo->flags & SS_F_DIR) != 0;
  s->dropwatch = (lo->flags & SS_F_DROPWATCH) != 0;
  s->time = lo->utc ? (long)(lo->utc + lora_age / 1000) : 0;
  s->tz = lo->tz_min;
  s->command_busy = lora_link_command_pending();
}

link_via_t link_merge_via(void)
{
  return via;
}

uint8_t link_path_to_cmd(const char *path, uint32_t *utc, int *tz_min)
{
  static const struct { const char *path; uint8_t cmd; } map[] =
  {
    { "/watching", CMD_WATCHING },
    { "/reelin",   CMD_REELIN },
    { "/ready",    CMD_READY },
    { "/resume",   CMD_RESUME },
    { "/skippull", CMD_SKIPPULL },
    { "/teachdir", CMD_TEACHDIR },
    { "/relearn",  CMD_RELEARN },
    { "/learn",    CMD_LEARN },
  };

  if(strncmp(path, "/settime?", 9) == 0)
  {
    const char *t = strstr(path, "t=");
    const char *z = strstr(path, "tz=");

    if(t == NULL || z == NULL)
      return 0;

    *utc = (uint32_t)strtoul(t + 2, NULL, 10);
    *tz_min = atoi(z + 3);
    return CMD_SETTIME;
  }

  for(unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if(strcmp(path, map[i].path) == 0)
      return map[i].cmd;

  return 0;
}

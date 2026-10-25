// =====================================================
// LoRa link for the StrikeSense watch
//
// SX1262 LoRa Node (HF) wired to the ESP32-S3-Touch-AMOLED-1.43.
// The backup for Bluetooth: when the rod is too far for Bluetooth
// (more than ~30 m) the watch gets the rod's status over LoRa
// (up to ~1 km on the beach). ble_link switches by itself.
// Runs in its own task (lora_link_start_task), so a Bluetooth scan
// never makes it miss a packet. Works from C and C++.
// =====================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "strikesense_protocol.h"

// 0 = no LoRa module on the watch (Bluetooth only)
#ifndef WATCH_LORA
#define WATCH_LORA 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  // ---------- Watch radio ----------
  bool     radio_ok;       // SX1262 answered at start-up
  int      init_error;     // RadioLib error code, 0 = OK

  // ---------- Link ----------
  bool     connected;      // heartbeat from the rod in the last 5 s
  uint32_t last_hb_ms;     // millis() of the last heartbeat, 0 = never
  uint32_t hb_count;       // heartbeats received
  uint32_t last_rx_ms;     // millis() of the last packet of any kind
  float    last_rssi;      // dBm (closer to 0 = stronger)
  float    last_snr;       // dB

  // ---------- Rod status (from the last heartbeat) ----------
  uint8_t  rod_state;      // ROD_xxx
  uint8_t  strike_type;    // STRIKE_xxx, STRIKE_NONE when no alarm
  uint16_t strike_id;      // changes with every new strike
  float    strike_peak;    // degrees, from the STRIKE packet
  uint8_t  sea;            // 0 calm, 1 moderate, 2 rough
  uint8_t  rod_battery;    // %, 255 = not measured
  uint8_t  learn_left;     // seconds
  float    tilt;           // degrees, + toward the water
  uint8_t  bucket;         // slow pull bucket %
  uint8_t  knocks;

  // ---------- rod 2.3+ (ext = false with an older rod) ----------
  bool     ext;
  float    ftilt;          // degrees, less smoothing
  float    allow;          // pull allowance, degrees
  uint8_t  drop;           // drop-back %
  uint8_t  flags;          // SS_F_xxx
  uint8_t  knock_id;       // low byte of the rod's knock number
  uint8_t  kpeak10;        // highest knock in the last heartbeat, x10 of the knock limit
  uint8_t  wave;
  uint8_t  wind;
  long     bat_left;       // minutes, -1 = still measuring
  float    ptdeg;
  float    ptneed;
  uint32_t utc;            // rod time, 0 = unknown
  int      tz_min;

  // ---------- rod 2.4: private LoRa, channels, crowd ----------
  bool     keyed;          // has our rod's key (paired over Bluetooth)
  uint8_t  chan;           // CH_xxx the watch listens on
  bool     searching;      // rod not heard: trying the channels
  uint8_t  quality;        // Q_xxx (from the rod)
  uint8_t  rods;           // other rods the rod hears
  bool     crowd;          // crowd mode
  uint8_t  loss;           // % of the rod's heartbeats missed lately
  int      lmode;          // the rod's link choice (0 auto, 1 BT only, 2 LoRa only), -1 unknown
  int      chm;            // the rod's channel choice CHM_xxx, -1 unknown
} lora_link_status_t;

// Call once in setup()
bool lora_link_begin(void);

// Call often (or use lora_link_start_task)
void lora_link_poll(void);

// begin + poll in a task of its own (call once in setup())
void lora_link_start_task(void);

lora_link_status_t lora_link_get_status(void);

// 0..4 bars from the last packet; 0 when nothing received for 6 s
uint8_t lora_link_signal_bars(void);

// Send a CMD_xxx to the rod; repeated until the rod confirms.
// Safe to call from the LVGL / UI code.
void lora_link_send_command(uint8_t cmd);
void lora_link_send_settime(uint32_t utc, int tz_min);
void lora_link_send_command_arg(uint8_t cmd, uint32_t arg);   // CMD_LINKMODE, CMD_CHANMODE

// True while a command is still waiting for the rod's confirmation
bool lora_link_command_pending(void);

const char* lora_link_strike_name(uint8_t strike_type);

// Our rod's LoRa key (from the rod over the encrypted Bluetooth: /lorakey).
// Kept in flash; without it the watch's LoRa stays quiet.
void lora_link_set_key(uint16_t rod_tag, const uint8_t key[16]);
bool lora_link_has_key(uint16_t rod_tag);       // already this rod's key?

const char* lora_link_chan_name(uint8_t chan);   // "500 m" / "1 km"
const char* lora_link_quality_name(uint8_t q);   // Clear / Good / Busy / Congested

#ifdef __cplusplus
}
#endif

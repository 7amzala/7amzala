// =====================================================
// LoRa link for the StrikeSense watch
//
// SX1262 LoRa Node (HF) wired to the ESP32-S3-Touch-AMOLED-1.43.
// Can be used from C (lcd_bsp.c) and C++ (.ino).
// =====================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "strikesense_protocol.h"

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
  uint32_t hb_count;       // heartbeats received (changes once per second)
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
} lora_link_status_t;

// Call once in setup()
bool lora_link_begin(void);

// Call often in loop()
void lora_link_poll(void);

lora_link_status_t lora_link_get_status(void);

// 0..4 bars from the last packet; 0 when nothing received for 6 s
uint8_t lora_link_signal_bars(void);

// Send a CMD_xxx to the rod; repeated until the rod confirms.
// Safe to call from the LVGL / UI code.
void lora_link_send_command(uint8_t cmd);

// True while a command is still waiting for the rod's confirmation
bool lora_link_command_pending(void);

const char* lora_link_strike_name(uint8_t strike_type);

#ifdef __cplusplus
}
#endif

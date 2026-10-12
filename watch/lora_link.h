// =====================================================
// LoRa link for the StrikeSense watch
//
// SX1262 LoRa Node (HF) wired to the ESP32-S3-Touch-AMOLED-1.43.
// Can be used from C (lcd_bsp.c) and C++ (.ino).
// =====================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  bool     radio_ok;     // SX1262 answered at start-up
  int      init_error;   // RadioLib error code, 0 = OK
  uint32_t packets;      // packets received
  uint32_t last_rx_ms;   // millis() of the last packet, 0 = none yet
  float    last_rssi;    // dBm (closer to 0 = stronger)
  float    last_snr;     // dB
} lora_link_status_t;

// Call once in setup()
bool lora_link_begin(void);

// Call often in loop()
void lora_link_poll(void);

lora_link_status_t lora_link_get_status(void);

// 0..4 bars from the last packet; 0 when nothing received for 6 s
uint8_t lora_link_signal_bars(void);

#ifdef __cplusplus
}
#endif

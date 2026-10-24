#ifndef LINK_MERGE_H
#define LINK_MERGE_H

// Bluetooth <-> LoRa: which link the screens use.
//
// Bluetooth while it is fresh (it has the live graph and the last strike).
// When Bluetooth is lost (rod more than ~30 m away) the rod's LoRa
// heartbeat fills the same status, so the screens, buzzes and buttons
// keep working. Back to Bluetooth as soon as it answers again.

#include <stdbool.h>
#include <stdint.h>

#include "ble_link.h"
#include "lora_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_BT_FRESH_MS    2500   // Bluetooth answers 5-10 times a second
#define LINK_LORA_FRESH_MS  3500   // LoRa heartbeat: 1-2 a second

// s: the Bluetooth status (in), the status the screens use (out)
void link_merge(ble_link_status_t *s, const lora_link_status_t *lo, uint32_t now_ms, bool take_peak);

// The link the last link_merge picked
link_via_t link_merge_via(void);

// "/watching" -> CMD_WATCHING ...; 0 = not a LoRa command
uint8_t link_path_to_cmd(const char *path, uint32_t *utc, int *tz_min);

#ifdef __cplusplus
}
#endif

#endif

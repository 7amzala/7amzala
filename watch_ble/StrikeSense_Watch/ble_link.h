#ifndef BLE_LINK_H
#define BLE_LINK_H

// Bluetooth (BLE) link from the watch to the rod.
//
// Finds the rod ("StrikeSense"), connects, asks for "/watch" 5 times a
// second and sends the buttons ("/watching", "/reelin", "/ready",
// "/skippull"). Reconnects by itself. Same requests as the phone app,
// with request numbers from 500000 so the two never mix.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  BLE_LINK_SEARCHING,
  BLE_LINK_CONNECTING,
  BLE_LINK_CONNECTED
} ble_link_phase_t;

typedef struct
{
  ble_link_phase_t phase;
  int rssi;                 // dBm, 0 = unknown
  uint32_t seq;             // +1 for every new answer from the rod
  uint32_t age_ms;          // since the last answer

  // from the rod ("/watch")
  char state[16];           // learning, pulltest, settling, monitoring, paused
  bool strike;
  char type[24];            // SLOW PULL, SHARP STRIKE ...
  long strikeid;
  long knockid;
  float energy_peak;        // highest knock energy since the screen last took it
  float knockthr;
  float tilt;               // deg, + toward the water
  float allow;
  float ptdeg;              // pull test: how far the tip moved
  float ptneed;             // pull test: how far it must move
  int bucket;               // slow pull %
  int drop;                 // drop-back %
  int knocks;               // knocks in the last 4 s
  int wave;                 // 0 flat .. 3 big
  int wind;                 // 0 calm .. 3 very strong
  int bat;                  // rod battery %, -1 = none
  bool dir;                 // pull direction learned
  bool dropwatch;           // drop-back seen, watching for a pull

  bool command_busy;        // a button is still being sent
} ble_link_status_t;

void ble_link_begin(void);
void ble_link_poll(void);                                   // call from loop()
void ble_link_get(ble_link_status_t *out, bool take_peak);  // safe from the screen task
void ble_link_command(const char *path);                    // safe from the screen task

#ifdef __cplusplus
}
#endif

#endif

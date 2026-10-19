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
  long batleft;             // rod battery minutes left, -1 = still measuring
  bool dir;                 // pull direction learned
  bool dropwatch;           // drop-back seen, watching for a pull

  bool command_busy;        // a button is still being sent

  // while looking (shown small on the screen, also on Serial)
  int scans;                // scans done
  int seen;                 // Bluetooth devices seen in the last scan
  char note[40];            // what the link is doing / what went wrong
} ble_link_status_t;

// The rod's last strike (LAST STRIKE page). Fresh after a rod restart.
#define LS_MAX 150

typedef struct
{
  bool loaded;              // an answer has come in
  int error;                // 0 = fine, else the rod's answer code (404 = rod sketch too old)
  bool have;                // there was a strike since the rod started
  bool recording;           // a new strike is being recorded (10 s after)
  char type[24];
  long ago_s;               // seconds ago, when it was fetched
  uint32_t got_ms;          // millis() when fetched
  int count;                // strikes since the rod started
  float tiltpeak;
  int pull;
  int pre;                  // points before the strike
  int n;                    // points
  short k[LS_MAX];          // knock, % of the knock limit
  short t[LS_MAX];          // tilt x 100
} ble_last_strike_t;

void ble_link_ask_last_strike(void);                        // safe from the screen task
bool ble_link_get_last_strike(ble_last_strike_t *out);      // copy; false = nothing yet

void ble_link_begin(void);
void ble_link_poll(void);                                   // call from loop()
void ble_link_get(ble_link_status_t *out, bool take_peak);  // safe from the screen task
void ble_link_command(const char *path);                    // safe from the screen task

#ifdef __cplusplus
}
#endif

#endif

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

// Which link the screens use (link_merge.c)
typedef enum
{
  LINK_NONE,
  LINK_BT,                  // Bluetooth: live graph, last strike, phone at the same time
  LINK_LORA                 // LoRa: rod too far for Bluetooth, status 1-2 times a second
} link_via_t;

typedef struct
{
  ble_link_phase_t phase;   // with LoRa in use: BLE_LINK_CONNECTED too
  link_via_t via;
  bool lora_ok;             // the watch's LoRa module answered
  int lora_bars;            // 0..4, 0 = rod not heard over LoRa
  int lora_chan;            // CH_xxx: 0 = 500 m fast, 1/2 = 1 km long range
  int lora_quality;         // Q_xxx: Clear, Good, Busy, Congested
  int lora_rods;            // other rods near (crowd)
  int lmode;                // the link choice kept on the rod: 0 auto, 1 Bluetooth only, 2 LoRa only, -1 unknown
  int chm;                  // the LoRa channel choice: CHM_xxx, -1 unknown (rod without LoRa)
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
  float ftilt;              // same, less smoothing (rod 2.2+): the ring follows the tip
  long time;                // the rod's time: UTC seconds since 1970, 0 = unknown
  int tz;                   // local time = UTC + tz minutes
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
#define LS_ZOOM 200         // 2 s before + 2 s after at 50 a second

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
  int zn;                   // ZOOM points (50 a second)
  int zpre;                 // ZOOM points before the strike
  short zk[LS_ZOOM];
  short zt[LS_ZOOM];
} ble_last_strike_t;

void ble_link_ask_last_strike(void);                        // safe from the screen task
bool ble_link_get_last_strike(ble_last_strike_t *out);      // copy; false = nothing yet

// ---------- pairing: the watch talks only to its own rod ----------
//
// Not paired yet: the watch lists the rods that are open for pairing
// (switched on in the last 3 min) by their ID, you pick yours and type its
// PIN (on the rod box). Then it only ever connects to that rod, encrypted.

#define PAIR_MAX 6

typedef struct
{
  char id[12];              // ROD-4F2A
  int rssi;
} pair_rod_t;

typedef enum
{
  PAIR_IDLE,
  PAIR_WORKING,             // connecting + checking the PIN
  PAIR_OK,                  // paired: restart into the normal screens
  PAIR_FAILED               // wrong PIN, or the rod is not open for pairing
} pair_state_t;

bool ble_link_is_paired(void);                              // (reads the flash; fine before ble_link_begin)
bool ble_link_rod_id(char *out, int size);                  // our rod's ID; false = not paired
int ble_link_pair_list(pair_rod_t *out, int max);           // rods open for pairing nearby
void ble_link_pair_start(const char *rod_id, uint32_t pin); // safe from the screen task
pair_state_t ble_link_pair_state(void);
void ble_link_forget(void);                                 // forget our rod (pair again)

void ble_link_begin(void);
void ble_link_poll(void);                                   // call from loop()
void ble_link_get(ble_link_status_t *out, bool take_peak);  // safe from the screen task
void ble_link_command(const char *path);                    // safe from the screen task

#ifdef __cplusplus
}
#endif

#endif

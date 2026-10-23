#ifndef STRIKE_LOG_H
#define STRIKE_LOG_H

// The watch records every strike itself, from the live data it already
// shows (10 points a second): 10 s before the strike until the fish has
// been quiet for 10 s (at most 3 minutes). The newest 20 are kept in
// memory, each only as big as it is (a typical strike is 2-4 KB); if
// memory runs short the oldest are dropped from memory (not from the
// card). strike_sd.cpp saves them on the SD card and loads them at start.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLOG_PRE    100                    // 10 s before the strike
#define SLOG_MAX    (SLOG_PRE + 1800)      // + up to 3 min after
#define SLOG_KEEP   20                     // in memory

typedef struct
{
  uint32_t utc;                            // when (UTC seconds, 0 = time not known)
  int tz;                                  // local time = utc + tz minutes
  uint32_t got_ms;                         // when, by the watch's own count (for "... ago" without time)
  long strikeid;
  char type[24];                           // SLOW PULL, SHARP STRIKE, ...
  float allow;                             // the rod's wave allowance then (deg)
  uint16_t n;                              // points (10 a second)
  uint16_t pre;                            // the strike is at this point
  bool done;                               // finished recording
  bool saved;                              // on the SD card
  bool from_card;                          // loaded from the SD card
  int16_t *k;                              // knock, % of the knock limit (0..300)
  int16_t *t;                              // tilt x 100
  uint8_t *st;                             // what the fish did: 0 quiet, 1 knock, 2 slack, 3 run
} slog_t;

// every 100 ms with the live data
void slog_tick(int k, int tilt100, int fish, bool strike, long strikeid, const char *type,
               float allow, bool answered, uint32_t now_ms, uint32_t utc, int tz);

int slog_count(void);                      // recordings (finished or recording now)
slog_t *slog_get(int i);                   // 0 = newest
bool slog_recording(void);

// for strike_sd.cpp
slog_t *slog_unsaved(void);                // a finished recording not on the card yet, or NULL
slog_t *slog_add_loaded(int n);            // room for one of n points loaded from the card (oldest first), or NULL
const char *slog_memory_note(void);        // for the Serial Monitor

#ifdef __cplusplus
}
#endif

#endif

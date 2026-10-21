#ifndef WATCH_CLOCK_H
#define WATCH_CLOCK_H

// Time and date on the watch. The phone tells the rod, the rod tells the
// watch; the watch keeps counting in its clock chip (PCF85063, keeps going
// after a restart while the battery is in) and tells a restarted rod.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void clock_begin(void);              // after Touch_Init() (shares its bus)
uint32_t clock_now(void);            // UTC seconds since 1970, 0 = not known yet
int clock_tz(void);                  // local time = UTC + this many minutes
void clock_set(uint32_t utc, int tz_min);

#ifdef __cplusplus
}
#endif

#endif

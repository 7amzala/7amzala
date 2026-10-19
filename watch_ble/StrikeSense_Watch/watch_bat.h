#ifndef WATCH_BAT_H
#define WATCH_BAT_H

// The watch's own battery: % and time left (BATTERY page).

#ifdef __cplusplus
extern "C" {
#endif

void watch_bat_begin(void);         // after Touch_Init() (uses its I2C bus)
void watch_bat_poll(void);          // from loop(): reads every 10 s
int watch_bat_percent(void);        // -1 = not measured
long watch_bat_left_min(void);      // -1 = still measuring

#ifdef __cplusplus
}
#endif

#endif

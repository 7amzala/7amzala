#ifndef STRIKE_SD_H
#define STRIKE_SD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool strike_sd_begin(void);      // at start, before the screens: finds the card, loads the newest 20 strikes
void strike_sd_save(void);       // saves finished strikes not on the card yet (call when no strike is on)
bool strike_sd_present(void);

#ifdef __cplusplus
}
#endif

#endif

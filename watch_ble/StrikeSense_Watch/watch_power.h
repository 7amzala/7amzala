#ifndef WATCH_POWER_H
#define WATCH_POWER_H

// Saving power while the screen is dark: the chip runs slower.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void power_screen(bool on);   // screen on: full speed (240 MHz); dark: 80 MHz

#ifdef __cplusplus
}
#endif

#endif

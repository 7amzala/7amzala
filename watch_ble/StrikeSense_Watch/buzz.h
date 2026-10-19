#ifndef BUZZ_H
#define BUZZ_H

// Wrist vibration: Pimoroni Haptic Bzzz (DRV2605L + LRA motor, I2C 0x5A)
// on its own I2C bus: SDA = header pin 43, SCL = header pin 44.
// Without the module the watch works the same (no vibration).

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  BUZZ_TAP,          // tiny click: button, swipe
  BUZZ_KNOCK,        // 1 short tick
  BUZZ_PULL,         // soft pulse (slow pull building)
  BUZZ_DROP,         // 2 short ticks
  BUZZ_STRIKE,       // strong long buzz
  BUZZ_LOST,         // 3 slow buzzes
  BUZZ_HELLO         // start-up: the module was found
} buzz_t;

bool buzz_begin(void);         // false = no module found
void buzz_play(buzz_t what);   // starts it and returns at once

#ifdef __cplusplus
}
#endif

#endif

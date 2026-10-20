// =====================================================
// POWER: the chip at 80 MHz while the screen is dark (enough for
// Bluetooth, the motion sensor and the vibration), 240 MHz when it is on.
// =====================================================

#include <Arduino.h>
#include "watch_power.h"

void power_screen(bool on)
{
  uint32_t want = on ? 240 : 80;

  if(getCpuFrequencyMhz() != want)
    setCpuFrequencyMhz(want);
}

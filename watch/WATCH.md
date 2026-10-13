# StrikeSense watch: setup

ESP32-S3-Touch-AMOLED-1.43 + SX1262 LoRa Node (HF), talking to the rod
(XIAO ESP32-S3 + Wio-SX1262) over LoRa.

## Files in the watch sketch folder

| File | What it is |
|---|---|
| `lcd_bsp.c` | Your screens, now driven by the real rod (replace your old one) |
| `lora_link.h`, `lora_link.cpp` | LoRa link to the rod |
| `strikesense_protocol.h` | Message format. **Must be identical** to the copy in the rod folder |
| your other files | `lcd_bsp.h`, `FT3168.*`, `lcd_config.h`, boot image… unchanged |

Library: **RadioLib** (Library Manager).

## Your watch .ino: 3 additions

```cpp
#include "lora_link.h"            // 1. at the top

void setup()
{
  Serial.begin(115200);
  // ... your existing touch + screen setup (lcd_lvgl_Init etc.) ...
  lora_link_begin();               // 2. after the screen setup
}

void loop()
{
  lora_link_poll();                // 3. keeps the radio running
  delay(2);
}
```

Arduino IDE: **Tools → USB CDC On Boot → Enabled** to see the `[LoRa]`
messages in the Serial Monitor.

## Wiring (watch ↔ LoRa module)

| LoRa pad | Watch | | LoRa pad | Watch |
|---|---|---|---|---|
| 3V3 | 3V3 | | RESET | IO6 |
| GND | GND | | BUSY | IO7 |
| CS | IO5 | | DIO1 | IO1 |
| CLK | IO16 | | RXEN | IO2 |
| MOSI | IO17 | | TXEN | IO3 |
| MISO | IO18 | | ANT, DIO2, other GND | not connected |

## Radio

869.525 MHz, SF7, 125 kHz, 14 dBm (EU 869.4–869.65 MHz band, 10% transmit
time allowed; one heartbeat per second uses about 5%).
`SS_NET_ID` in `strikesense_protocol.h` must be the same on rod and watch.

## How a session works

| Watch screen | Button | What the rod does |
|---|---|---|
| CONNECTING | – | Watch waits for the rod's heartbeat (1 per second) |
| ENVIRONMENT SETUP | START LEARNING | Learns 15 s, then pauses |
| LEARNING COMPLETE | READY TO CAST | (watch counts down the cast) |
| CAST MODE | READY NOW / countdown ends | Settles, then asks for the pull test |
| PULL TEST | pull the line once, or SKIP | Learns the direction, arms itself |
| MONITORING | ADJUST ROD | Pauses |
| ADJUST ROD | READY | Settles, arms itself (no pull test) |
| STRIKE! (type shown) | I'M WATCHING | Stops the alarm, learns the new position |
| STRIKE! | REEL IN | Pauses |
| REEL IN | READY TO CAST | (cast countdown, then like above) |

- Strikes are repeated by the rod until the watch confirms; they also show
  in every heartbeat, so the watch knows within about 1 second even if a
  packet is lost.
- Watch buttons are repeated until the rod confirms.
- No heartbeat for 15 s: the watch shows **ROD NOT FOUND** and reconnects by
  itself when the rod is heard again.
- **SIM STRIKE** on the monitoring screen is still a local demo (no radio).
- Swipe left: **ROD LINK** page with real signal bars, RSSI and SNR.
- The watch also sends a small "I'm here" message every 5 s, so the rod's
  web dashboard can show **WATCH CONNECTED** with signal bars.

## Tested on the PC

`sim/link_test.cpp` runs the real rod sketch and the real watch link
together over a virtual radio, through a whole session, with 0% and 30% of
packets lost: 44/44 checks pass. Not yet tested on the real boards.

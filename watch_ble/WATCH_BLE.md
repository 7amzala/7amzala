# StrikeSense watch – Bluetooth version

Waveshare ESP32-S3-Touch-AMOLED-1.43 talking to the rod over **Bluetooth**
(no LoRa module needed). The phone app can be connected at the same time.

## Screens

| Screen | Shows | Buttons |
|---|---|---|
| Looking for the rod | searching / connecting | – |
| Rod armed / Knock / Slow pull / Drop-back | live graph (knock top, tilt bottom, last 20 s), tilt, knocks or pull %, rod battery, signal | REEL IN |
| STRIKE! | red, flashing, strike type | I'M WATCHING, REEL IN |
| Reeled in | cast, then press READY | READY |
| Pull test (only the first time, or after NEW PULL DIRECTION) | what to do, progress bar | SKIP |

Outer ring: cyan = all fine, orange = something building, red = strike.

### Swipes

| From | Swipe | Page |
|---|---|---|
| main | left | SEA STATE (waves, wind) |
| main | right | SETUP: RELEARN THE SEA, NEW PULL DIRECTION (hold 2 s each), TEACH MY LOOK |
| any page | down | LAST STRIKE: 20 s before + 10 s after, from the rod (fresh after a rod restart) |
| any page | up | BATTERY: rod + watch %, time left (learned after ~15 min) |

The opposite swipe goes back. A strike jumps to the main page; after
30 s on another page the watch goes back by itself. The watch battery is
read from an AXP2101 chip if there is one, else GPIO 4 (Serial: `[BAT] ...`).

## Setup (Arduino IDE)

1. Open `StrikeSense_Watch/StrikeSense_Watch.ino` (all files in that folder
   belong together: the screen and touch drivers are the same as the
   Waveshare demo).
2. Library: **lvgl 8.3.x** (Library Manager), as for the Waveshare demo.
3. `lv_conf.h`: use the one here (copy it next to the `lvgl` folder in your
   Arduino `libraries` folder), or in yours switch on the bigger fonts:
   `LV_FONT_MONTSERRAT_28`, `_36`, `_48` (16 and 20 too). Without them the
   watch still works, with smaller titles.
4. Tools: Board **ESP32S3 Dev Module**, Flash Size **16MB**, Partition Scheme
   **Huge APP (3MB No OTA/1MB SPIFFS)**, PSRAM **OPI PSRAM**,
   USB CDC On Boot **Enabled**.
5. Upload. The Serial Monitor (115200) shows `[BLE] ...` messages.

The rod must run the Bluetooth sketch (it takes the phone and the watch at
the same time). Range: about 30 m.

## Tested on the PC

`sim/watch_ui_test`: the real `watch_ui.c` with the real LVGL 8.3.11 and a
scripted rod: every screen drawn, every button tapped (all send the right
request). `sim/ble_test.cpp`: the rod with phone + watch connected at once.
Not yet tested on the real watch.

## Watch stuck on "LOOKING FOR THE ROD"

The small text under the title says what is happening:

| Watch shows | Meaning | Fix |
|---|---|---|
| `scan N - X devices seen` / `rod not seen yet` | the rod is not advertising | upload the newest rod sketch; on the rod's Serial you should see `Bluetooth: advertising, 1 connected` every 5 s. `full (not advertising)` = 2 already connected |
| `rod found but could not connect` | too far, or rod busy | come closer, restart the rod |
| `no StrikeSense service on rod` | very old rod sketch | upload the newest rod sketch |
| `rod sketch too old - update it` / `connected, rod not answering` | rod has no `/watch` | upload the newest rod sketch |

Watch Serial (115200) lists every named device it sees (`[BLE]   saw ...`)
and marks the rod with `<- THE ROD`.

## Wrist vibration (optional): Pimoroni Haptic Bzzz

| Bzzz pin | Watch header pin |
|---|---|
| 2-5V | 3V3 |
| GND | GND |
| SDA | SDA |
| SCL | SCL |
| TRIG | not connected |

| Event | You feel |
|---|---|
| Knock | 1 tick |
| Slow pull building (from 10%) | soft pulse every 2 s, faster as it grows |
| Drop-back | 2 ticks |
| STRIKE | strong buzz every 1.5 s until you press I'M WATCHING or REEL IN |
| Rod lost | 3 slow buzzes |
| Button / swipe | tiny click |

At start the watch gives a double click and the Serial Monitor shows
`[BUZZ] found`. Without the module: `[BUZZ] not found`, everything else
works as before. It shares the I2C bus with the touch
screen (different address, no clash).

## Screen: on while you look

The screen is on while you look at the watch and goes off as soon as you
look away (QMI8658 accelerometer + gyroscope, `[IMU] ...` on the Serial
Monitor). It only turns on after a real raise of the wrist, so lying face
up on a table does not count, and lying perfectly still for 2 s turns it
off. Also on: while a slow pull / drop-back builds, during a strike until
you answer it, in the pull test, and 10 s after a touch (that touch only
wakes it). A knock only vibrates.

TEACH MY LOOK (RELEARN page, swipe right): hold the watch the way you look
at it for 3 s; the watch keeps that angle (also after a restart).

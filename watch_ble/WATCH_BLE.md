# StrikeSense watch – Bluetooth version

Waveshare ESP32-S3-Touch-AMOLED-1.43 talking to the rod over **Bluetooth**,
with an optional **LoRa** backup when the rod is far (see below). The phone
app can be connected at the same time.

## Screens

| Screen | Shows | Buttons |
|---|---|---|
| Looking for the rod | searching / connecting | – |
| Rod armed / Knock / Slow pull / Drop-back | live graph (knock top, tilt bottom, last 20 s), tilt, knocks or pull %, rod battery, signal | REEL IN |
| STRIKE | what the fish does now: RUN / KNOCK / SLACK / QUIET m:ss, trend arrow, strength ring, PULL % and TILT, live graph with the strike marked, 30 s story bar | WATCHING (eye), REEL IN |
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
3. LVGL settings: nothing to copy. `build_opt.h` in the sketch folder gives
   LVGL its settings (64 KB memory, fonts) and any `lv_conf.h` in your
   libraries folder is ignored. Keep `build_opt.h` next to the .ino.
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

## First start: pair the watch with YOUR rod (private, encrypted)

Every rod has its own ID and PIN (the rod prints them on its Serial
Monitor at start: `==== ROD-4F2A   PIN 482913 ====`, write both on the box).

1. Switch the rod on (for 3 minutes it shows its ID; a brand-new rod until
   its first pairing).
2. The watch shows **PAIR YOUR ROD** with the rods nearby: tap your ID, type
   the PIN, tick.
3. "Paired": the watch restarts and from then on talks **only to that rod**,
   encrypted (Bluetooth and LoRa). Other rods nearby are ignored.

The phone app pairs the same way (its own pairing page). In the app:
Settings > MY ROD > **Forget my devices** makes the rod forget the phone and
the watch (new phone, lost watch); then pair again with the PIN.

**Forget the rod on the watch:** SETUP > hold **Watch link** for 5 s (a tap
still changes the link). The watch forgets the rod and its LoRa key and
restarts into PAIR YOUR ROD, which asks for the PIN. A watch always pairs
over Bluetooth with the PIN first; LoRa works only after that (the key
comes over the encrypted Bluetooth). **LoRa only** is remembered after a
restart: the watch does not try Bluetooth then.

**LVGL settings:** come from `build_opt.h` in the sketch folder (64 KB
memory); no `lv_conf.h` to copy.

## LoRa backup (optional): far from the rods

With an **SX1262 LoRa Node (HF, 868 MHz)** wired to the watch and the
**LoRa rod sketch** (`StrikeSense/StrikeSense.ino`, rod 2.3 with the
Wio-SX1262), the watch switches by itself:

| Rod distance | Link | Watch shows |
|---|---|---|
| up to ~30 m | Bluetooth | cyan signal bars, smooth live graph, LAST STRIKE from the rod |
| ~30 m to ~1 km | LoRa | orange bars + "LoRa" next to the rod battery; ring, alarms, buttons, vibration all work; graph and ring update 1-2 times a second |
| back near | Bluetooth again | (within a few seconds) |

The phone app stays on Bluetooth. Strike history on the watch works on both.

### Wiring (watch ↔ LoRa module)

| LoRa pad | Watch | | LoRa pad | Watch |
|---|---|---|---|---|
| 3V3 | 3V3 | | RESET | IO6 |
| GND | GND | | BUSY | IO7 |
| CS | IO5 | | DIO1 | IO1 |
| CLK | IO16 | | RXEN | IO2 |
| MOSI | IO17 | | TXEN | IO3 |
| MISO | IO18 | | ANT | the 868 MHz antenna (never transmit without it) |

Library: **RadioLib** (Library Manager). The micro-SD card now runs in SD
mode (same slot, nothing to rewire), so the SPI bus is free for LoRa.
No module wired? The watch works as before over Bluetooth ("LoRa module
not found" under "Looking for the rod"). To switch LoRa off completely set
`WATCH_LORA 0` in `lora_link.h`.

Private: every LoRa message is encrypted and signed with your rod's own
key (the watch gets it during pairing), so 5 or 40 rods on a beach never
mix. Channels (Auto, or chosen in the app / on the watch's SETUP page):

| Channel | Range | News on the watch | Rule |
|---|---|---|---|
| 500 m fast (869.85 MHz, 250 kHz, 5 mW) | ~500 m | 1 a second calm, 3-4 when something happens | no time limit |
| 1 km A / B (869.4625 / 869.5875 MHz, 125 kHz, 25 mW) | ~1 km | about 1 a second | 10% transmit time |

Auto starts on 500 m, moves to 1 km when the watch hears the rod badly or
not at all, and back after a strong minute. Many rods heard: crowd mode
(calm news less often, strikes still first). The watch shows e.g.
`78% LoRa 500m Clear`; the app: `Watch LoRa · 500 m · Clear`.
Rod and watch must have the same `strikesense_protocol.h` and `ss_crypto.c/.h`.

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

## Strike vibrations (until you answer)

| Fish | You feel |
|---|---|
| RUN (pulling the line) | long buzz, every 1.5 s |
| KNOCK | a tick for each knock |
| SLACK (line went loose) | double tick, every 3 s |
| QUIET | one soft tick when it stops |
| not answered | a reminder buzz every 30 s |

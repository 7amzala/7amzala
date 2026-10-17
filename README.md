# Laarif StrikeSense

Fishing bite alarm for sea fishing: an ESP32 with a LIS3DH accelerometer on
the rod tip, plus a live dashboard over Wi-Fi.

## Files

- `StrikeSense/StrikeSense.ino`: the sketch with the LoRa watch link (open this folder in the Arduino IDE)
- `StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino`: the same without LoRa (dashboard only)
- `StrikeSense/TUNING.md`: which number to change for which problem
- `sim/`: PC simulator used to test the detection before going to the sea

## Hardware / libraries

- ESP32 board (Arduino core for ESP32)
- Sensor on I2C (SDA = D4, SCL = D5), found automatically at start-up:
  - **ADXL355** (EVAL-ADXL355-PMDZ), about 10x less noise. Pins 1 -> D5,
    2 -> D4, 5 -> GND, 6 -> 3V3, and on the sensor link 3 -> 12 and
    4 -> 11 (see `docs/adxl355_wiring.png`). Test it first with
    `rod_adxl355_test/`.
  - or **LIS3DH**, address `0x19`
- Library: **SparkFun LIS3DH Arduino Library** (needed to build, even with the ADXL355)
- `Preferences`, `WiFi` and `WebServer` come with the ESP32 core

## Using it

1. Power on. Connect your phone to Wi-Fi **StrikeSense-Test** (password `12345678`)
   and open **http://192.168.4.1**.
2. Tap **ENABLE SOUND** (this also keeps the phone screen on).
3. **LEARNING (15 s):** keep the rod still in the rest.
4. **PULL TEST:** pull the line gently toward the water once, then let go.
   The dashboard shows how far the tip moved and how far it needs to move.
   **SKIP** reuses the last saved direction.
5. **ROD ARMED:** it is watching.

After an alarm:

- **I'M WATCHING:** stops the alarm and learns the new rod position (max 8 s).
  Detection stays on.
- **REEL IN:** pauses detection. Press **READY** when the rod is back in
  the rest; you get a few seconds to place it, then a new pull test.

## What it detects

| Alarm | What the rod tip does |
|---|---|
| Knock (one short beep) | One short tap |
| REPEATED BITE | 3 taps within 4 s |
| SHARP STRIKE | A very strong hit, or a strong tap plus a fast pull toward the water |
| SLOW PULL | Tip pulled toward the water and staying there |
| DROP-BACK + PULL | Tip springs back (line goes slack), then pulls forward past its normal position: the fish pattern |

A **drop-back alone is not an alarm** (waves can loosen the braid). The
dashboard shows "Drop-back seen" and watches 2 minutes for a pull forward;
if none comes, the slack line becomes the new normal position.

The Conditions card shows waves, wind and tide, and picks CALM / MODERATE /
ROUGH automatically. Tap a sea state to override it, or AUTO to go back.

The header shows running time: **ON** (since power on), **ARMED** (since the
rod was last armed) and **TOTAL** (all hours ever, saved every 10 minutes).

**📡 WATCH** (next to the times) shows the LoRa link to the watch: signal bars,
GOOD / FAIR / WEAK / POOR and the signal in dBm, or **NOT CONNECTED** if the
watch has not been heard for 15 s. Hold the mouse on it (or long-press) to see
the signal in both directions.

| Bars | Quality | Signal (dBm) |
|---|---|---|
| 4 | GOOD | above -80 |
| 3 | FAIR | -80 to -95 |
| 2 | WEAK | -95 to -108 |
| 1 | POOR | below -108 |

**3D NOISE** (next to LIVE / LAST STRIKE) shows how the tip shakes as a 3D
cloud of dots (100 per second, last 3 s), with the knock limit as an orange
sphere. Dots inside = normal noise, dots outside = a knock. Drag to turn it.

**⚙ SETTINGS** opens live sliders (knock limit, repeated bite, sharp strike,
slow pull sensitivity, pull angle, pull time, drop-back + pull and knock beep
on/off). **Slow pull sensitivity** (x0.8 to x3) works in waves too: pull angle
is only a minimum, which waves usually exceed. Changes work
immediately; **SAVE** keeps them after power off.

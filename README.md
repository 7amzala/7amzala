# Laarif StrikeSense

Fishing bite alarm for sea fishing: an ESP32 with a LIS3DH accelerometer on
the rod tip, plus a live dashboard over Wi-Fi.

## Files

- `StrikeSense/StrikeSense.ino`: the sketch (open this folder in the Arduino IDE)
- `StrikeSense/TUNING.md`: which number to change for which problem
- `sim/`: PC simulator used to test the detection before going to the sea

## Hardware / libraries

- ESP32 board (Arduino core for ESP32)
- LIS3DH on I2C, address `0x19`
- Library: **SparkFun LIS3DH Arduino Library**
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
| DROP-BACK | Tip springs back away from the water and stays there (line goes slack) |

The Conditions card shows waves, wind and tide, and picks CALM / MODERATE /
ROUGH automatically. Tap a sea state to override it, or AUTO to go back.

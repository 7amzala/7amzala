# StrikeSense tuning guide

## Start with the live settings

Tap **⚙ SETTINGS** on the dashboard. Changes work immediately; the panel
shows the live knock and tilt next to their limits, and the dashed lines on
the graph move as you slide. Press **SAVE** to keep them after power off,
**DEFAULT** to go back.

| Problem | Slider | Move it |
|---|---|---|
| A small drag of the line gives SLOW PULL | **Pull angle** and/or **Pull time** | right (e.g. 1.0° → 1.5°, 2 s → 3 s) |
| Real slow pulls detected too late / missed | **Pull angle** / **Pull time** | left |
| Knock beeps with no fish | **Knock limit** | right (stricter) |
| Small knocks missed | **Knock limit** | left (more sensitive) |
| REPEATED BITE too easy / too hard | **Repeated bite** (count, seconds) | more knocks or fewer seconds = harder |
| SHARP STRIKE from hits that aren't fish / missed real strikes | **Sharp strike** | right = harder hit needed / left |
| Drop-back alarms not wanted | **Drop-back alarm** | OFF |
| Single knock beeps annoying | **Single knock beep** | OFF (the KNOCK box still flashes) |

How to test a slider: watch **TILT / PULL LINE** in the panel while you
drag the line. The alarm needs the tilt to stay above the pull line for
the pull time.

## Deeper tuning in the code

The other numbers are in the **TUNING** section at the top of
`StrikeSense.ino`. Change one number at a time, test, then change the next.

## Before you tune: use the replay

After every alarm, press **LAST STRIKE** on the graph. It shows 4 s before
and 4 s after the alarm:

- **Top (green):** knock energy. The orange dashed line is the knock limit.
- **Bottom (blue):** tilt in degrees, + toward the water, - away.
  The orange dashed lines are the wave allowance.

If the alarm was false, the replay shows what fooled it. If a fish was
missed, watch the live graph during the next bite and compare it with the
dashed lines.

## Problem → what to change

| Problem | Change | Direction |
|---|---|---|
| Single knock beeps from wind / rain | `knockDev` in `SEA_PROFILES` (1st number) | higher (e.g. 6 → 7) |
| Small knocks are missed | `knockDev` | lower (e.g. 6 → 5) |
| | `KNOCK_FLOOR` | lower (0.010 → 0.007) |
| Wind gusts give knock beeps | `GUST_NOISE_MUL` | higher (1.0 → 1.5) |
| Knock beeps with no fish, even when still | `knockDev` and `KNOCK_CONFIRM` | higher (3 → 4) |
| Sharp strike not detected (besides the slider) | `sharpAlone` (6th number) | lower |
| False SLOW PULL / DROP-BACK in waves | `bucketMul` (3rd number) | higher (2.2 → 2.6) |
| | `allowMul` (2nd number) | higher (0.6 → 0.7) |
| | `SUSTAIN_PERIOD_PART` | higher (0.6 → 0.7) |
| Slow pulls detected too late / missed | `bucketMul` | lower (2.2 → 1.9) |
| | `allowMul` | lower |
| Pull time feels longer in rough sea | `holdMul` (4th number) | lower (1.5 → 1.2) |
| Alarm after a slow tide change | `BASELINE_TAU` | lower (60 → 40) |
| Pull test is too hard in waves | `PULLTEST_MIN_DEG` | lower, or press SKIP |
| Sea state label changes too often | `SEA_STATE_HOLD` | higher |
| Labels (Small / Medium / Big …) feel wrong | `WAVE_*`, `WIND_*`, `TIDE_*` | adjust to what you see |
| Wind shows Strong while the sensor is still | changed sensor board? set `FORGET_SAVED_NOISE` to `true`, upload, then back to `false` | |

Profile order in `SEA_PROFILES`:
`knockDev, allowMul, bucketMul, holdMul, noDirAllowMul, sharpAlone, sharpWithPull`
for CALM, MODERATE and ROUGH. Only the row for the current sea state is used.

## Sensor noise

The bottom line of the Conditions card shows **sensor noise** (the quietest
vibration level ever measured, saved on the ESP32) and whether the LIS3DH
**high-res** mode is on. The wind label only counts vibration *above* this
noise, so a still sensor shows Calm.

- **high-res OFF:** the sensor stayed in its noisier mode. Tell me; the
  Serial monitor at startup prints `CTRL_REG1` and `CTRL_REG4` for checking.
- The knock limit is set from the noise, so lower noise = smaller knocks
  detected. With 0.011 g noise the limit is about 0.017 g, which means hits
  of roughly 0.06 g at the tip.

## How the slow-pull "bucket" works

- When the tip leans toward the water by more than the **allowance** (the
  **Pull angle** setting, or more in waves), the bucket fills. When it leans
  less, it drains.
- Waves fill it a little and drain it again. The code measures how full the
  waves alone get it, and makes the bucket **`bucketMul` times bigger** than that.
- An alarm needs a **full bucket** that has also stayed filled for the
  **Pull time** setting, and in waves longer than a wave can keep it filled
  (`SUSTAIN_PERIOD_PART` × wave period).
- Drop-back is the same bucket in the other direction.

## Simulator results (before testing at sea)

The sketch was run in a PC simulator with simulated waves, wind, tide and
fish, using the **real sensor noise measured on your LIS3DH** (0.0146 g
with the old settings, 0.011 g with the new averaging).
These are simulated results, not real-world ones.

| Sea | False alarms, 2 h without fish (×3 runs) | Knocks / repeated / sharp | Slow pull | Drop-back |
|---|---|---|---|---|
| Still / calm (±0.15°) | 0 (0–1 knock beeps) | 0.06 g knocks detected | 2° pull: ~6 s | ~6 s |
| Moderate (±1°, 6 s) | 0 | 0.06 g knocks detected | 3° pull: ~6 s | ~4–5 s |
| Rough (±2.5°, 8 s) | 0 | 0.06 g knocks partly; 0.3 g hit detected | 5° pull: ~9 s | 5°: ~9 s |
| Very rough (±4°, 10 s) | 0 | 0.3 g hit detected | 6° pull: missed | 6°: missed |

Small drags with the default settings (calm): 0.8° for 1 s, 1.0° for 1.5 s
and 1.5° for 1 s give **no alarm**; 1.5° held 4 s, 2° and 3° pulls alarm
after 4 s.

Known limits:

- In rough water a slow pull must be clearly bigger than the waves, and the
  alarm comes later (it must outlast a wave).
- If the sea goes from calm to rough within about 10 minutes (a squall),
  expect a few false alarms and knock beeps while it adapts. Pressing
  **ROUGH** in the Conditions card helps straight away.
- Without a pull test (direction not set), drop-backs can't be detected and
  slow pulls need to be bigger.

## Re-running the simulator

Needs a PC with `g++` (Linux / macOS / WSL):

```
cd sim
g++ -O2 -std=c++17 -Imock -o scenarios scenarios.cpp && ./scenarios
g++ -O2 -std=c++17 -Imock -o long_run long_run.cpp && ./long_run
g++ -O2 -std=c++17 -Imock -o weather_change weather_change.cpp && ./weather_change
```

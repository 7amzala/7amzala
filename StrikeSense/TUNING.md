# StrikeSense tuning guide

All numbers are in the **TUNING** section at the top of `StrikeSense.ino`.
Change one number at a time, test, then change the next.

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
| Wind gusts give knock beeps | `GUST_NOISE_MUL` | higher (3.0 → 3.5) |
| Repeated bite alarm too easy | `REPEAT_COUNT` | 3 → 4 |
| | `REPEAT_WINDOW_MS` | 4000 → 3000 |
| Sharp strike not detected | `SHARP_ABS_G` | lower (0.10 → 0.08) |
| | `sharpAlone` (6th number) | lower |
| Sharp strike false alarms | `SHARP_ABS_G` | higher |
| False SLOW PULL / DROP-BACK in waves | `bucketMul` (3rd number) | higher (2.2 → 2.6) |
| | `allowMul` (2nd number) | higher (0.6 → 0.7) |
| | `SUSTAIN_PERIOD_PART` | higher (0.6 → 0.7) |
| Slow pulls detected too late / missed | `bucketMul` | lower (2.2 → 1.9) |
| | `allowMul` | lower |
| Slow pulls missed in calm water | `PULL_MIN_ALLOW` | lower (0.40 → 0.30) |
| | `BUCKET_MIN` | lower (0.80 → 0.6) |
| Alarm after a slow tide change | `BASELINE_TAU` | lower (60 → 40) |
| Pull test is too hard in waves | `PULLTEST_MIN_DEG` | lower, or press SKIP |
| Sea state label changes too often | `SEA_STATE_HOLD` | higher |
| Labels (Small / Medium / Big …) feel wrong | `WAVE_*`, `WIND_*`, `TIDE_*` | adjust to what you see |

Profile order in `SEA_PROFILES`:
`knockDev, allowMul, bucketMul, holdMin, noDirAllowMul, sharpAlone, sharpWithPull`
for CALM, MODERATE and ROUGH. Only the row for the current sea state is used.

## How the slow-pull "bucket" works

- When the tip leans toward the water by more than the **allowance**, the
  bucket fills. When it leans less, it drains.
- Waves fill it a little and drain it again. The code measures how full the
  waves alone get it, and makes the bucket **`bucketMul` times bigger** than that.
- An alarm needs a **full bucket** that has also **stayed filled longer than a
  wave can keep it filled** (`SUSTAIN_PERIOD_PART` × wave period).
- Drop-back is the same bucket in the other direction.

## Simulator results (before testing at sea)

The sketch was run in a PC simulator with simulated waves, wind, tide and fish.
These are simulated results, not real-world ones.

| Sea | False alarms, 2 h without fish (×3 runs) | Knocks / repeated / sharp | Slow pull | Drop-back |
|---|---|---|---|---|
| Calm (±0.15°) | 0 | detected | 1.5° pull: ~4 s | ~4 s |
| Moderate (±1°, 6 s) | 0 | detected | 3° pull: ~5 s | ~5 s |
| Rough (±2.5°, 8 s) | 0 | 0.06 g knocks only partly; others detected | 5° pull: ~8–10 s | 5°: detected; 3.5°: missed |
| Very rough (±4°, 10 s) | 0 | detected | 6° pull: missed | 6°: missed |

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

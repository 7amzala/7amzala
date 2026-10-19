# StrikeSense Android app

The rod's dashboard as a real Android app, plus a strike alarm that works
with the phone locked.

- Connects to the rod over **Bluetooth** (BLE) by itself: no WiFi, the
  phone keeps its internet.
- The dashboard page is built into the app (copied from
  `StrikeSense_NoLoRa.ino` at every build); its requests (/data, /set ...)
  go to the rod over Bluetooth.
- Watches the rod in the background: strike = alarm sound (loops) +
  vibration + notification with **I'M WATCHING** and **SILENCE**.
- Warns when the connection is lost (30 s) or the rod battery is low.
- Help page with Bluetooth / permission buttons while the rod is not found.

## Install

1. On the phone, download `release/StrikeSense.apk` (or the
   "app-latest" release on GitHub).
2. Open it. Android asks once to allow installing apps from this source:
   allow it, then **Install**.
3. Open **StrikeSense**, allow **Nearby devices** (Bluetooth),
   notifications, and running in the background (battery).

## Build

GitHub builds it automatically (`.github/workflows/android.yml`) whenever
something in `android/` changes. Plain Java, no libraries.

`app/strikesense.keystore` is the app's signing key: updates must be signed
with the same key to install over the old version.

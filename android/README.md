# StrikeSense Android app

The rod's dashboard as a real Android app, plus a strike alarm that works
with the phone locked.

- Full screen dashboard from the rod (http://192.168.4.1), same as the browser.
- Stays on the rod's WiFi even though it has no internet.
- Watches the rod in the background: strike = alarm sound (loops) +
  vibration + notification with **I'M WATCHING** and **SILENCE**.
- Warns when the connection is lost (30 s) or the rod battery is low.
- Help page with a WiFi button when the rod can't be found.

## Install

1. On the phone, download `release/StrikeSense.apk` (or the
   "app-latest" release on GitHub).
2. Open it. Android asks once to allow installing apps from this source:
   allow it, then **Install**.
3. Open **StrikeSense**, allow notifications, and allow it to run in the
   background (battery).

## Build

GitHub builds it automatically (`.github/workflows/android.yml`) whenever
something in `android/` changes. Plain Java, no libraries.

`app/strikesense.keystore` is the app's signing key: updates must be signed
with the same key to install over the old version.

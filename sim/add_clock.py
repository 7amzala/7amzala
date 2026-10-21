#!/usr/bin/env python3
"""Rod: knows the time and date while switched on (it has no clock chip,
so after a restart it waits for the phone or the watch to tell it).

  /settime?t=<seconds since 1970, UTC>&tz=<minutes from UTC, e.g. 60>
  /watch (live data) gets "time" (0 = unknown) and "tz"

    python3 add_clock.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"{path}: expected {count} x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


rep('''// ---------- Saved setups (5, named, in flash) ----------''', '''// ---------- Time and date (from the phone or the watch) ----------

bool clockSet = false;
unsigned long clockEpochAt = 0;          // seconds since 1970 (UTC)...
unsigned long clockMillisAt = 0;         // ...at this millis()
long clockTzMin = 0;                     // local time = UTC + this

unsigned long nowEpoch()
{
  if(!clockSet)
    return 0;

  return clockEpochAt + (millis() - clockMillisAt) / 1000;
}

void handleSetTime()
{
  long t = server.arg("t").toInt();
  long tz = server.arg("tz").toInt();

  if(t < 1600000000L || tz < -14 * 60 || tz > 14 * 60)
  {
    server.send(400, "text/plain", "BAD TIME");
    return;
  }

  bool first = !clockSet;

  clockEpochAt = (unsigned long)t;
  clockMillisAt = millis();
  clockTzMin = tz;
  clockSet = true;

  if(first)
    Serial.printf("TIME SET: %lu (UTC%+ld min)\\n", (unsigned long)t, tz);

  server.send(200, "text/plain", "OK");
}

// ---------- Saved setups (5, named, in flash) ----------''')

rep('''  addField(json, "ft", fastTiltDeg, 2);''', '''  addField(json, "ft", fastTiltDeg, 2);
  addField(json, "time", (long)nowEpoch());
  addField(json, "tz", clockTzMin);''')

rep('''  server.on("/presets", handlePresets);''', '''  server.on("/settime", handleSetTime);
  server.on("/presets", handlePresets);''')

open(path, 'w').write(s)
print("ok", path)

#!/usr/bin/env python3
"""Rod: strike and knock alerts go out at once.

The live message went out on a 100 ms tick, so an alert could wait up to
100 ms on the rod before leaving. Now a strike or a knock sends it at
once to every line (phone and watch together).

    python3 add_instant_alert.py <sketch.ino>      (edits in place)
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


rep('''  startWatchRecording();
  fightOnStrike();
''', '''  startWatchRecording();
  fightOnStrike();

#if defined(ESP32)
  server.pushLive();                    // phone + watch hear it now, not at the next tick
#endif
''')

rep('''void onKnock(bool settlingMode)
{
''', '''void onKnock(bool settlingMode)
{
#if defined(ESP32)
  server.pushLive();                    // the knock shows at once on phone + watch
#endif
''')

open(path, 'w').write(s)

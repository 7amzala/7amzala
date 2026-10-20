#!/usr/bin/env python3
"""Pull test only once: when the rod knows no pull direction yet.

The direction from a pull test is kept in flash and re-aimed when the rod
sits at a new angle (the sensor is always mounted the same way). So after
switching on, after READY (casting) and after RELEARN there is no pull test
any more - only the very first time, or when you ask for one:
"/teachdir" (phone settings, watch RELEARN page: NEW PULL DIRECTION).

Run after add_live_broadcast.py.

    python3 add_pulltest_once.py <sketch.ino>      (edits in place)
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


rep('''bool pullTestAfterSettle = false;''', '''bool pullTestAfterSettle = false;
bool teachDirAsked = false;      // "new pull direction" asked: pull test after learning''')

# after learning: only without a known direction (or when asked).
# (the learning step ends with "startPullTest();" at function level)
rep('''

  startPullTest();
}
''', '''

  // The pull direction is kept from the first pull test: no test again
  if(haveDir && !teachDirAsked)
  {
    Serial.println("PULL DIRECTION: the saved one (no pull test)");
    goMonitoring();
    return;
  }

  teachDirAsked = false;
  startPullTest();
}
''')

# after READY (casting): only without a known direction
rep('''  if(pullTestAfterSettle)
  {
    pullTestWaveDeg = 0;
    startPullTest();
  }''', '''  if(pullTestAfterSettle && !haveDir)
  {
    pullTestWaveDeg = 0;
    startPullTest();
  }''')

# "/teachdir": a pull test now (or right after learning)
rep('''void handleSkipPull()
{''', '''// NEW PULL DIRECTION (phone settings / watch): one pull test now
void handleTeachDir()
{
  if(rodState == ST_LEARNING)
  {
    teachDirAsked = true;             // right after learning
  }
  else
  {
    Serial.println("NEW PULL DIRECTION ASKED");
    pullTestWaveDeg = 0;
    startPullTest();
  }

  server.send(200, "text/plain", "OK");
}

void handleSkipPull()
{''')

rep('''  server.on("/watch", handleWatch);''', '''  server.on("/watch", handleWatch);
  server.on("/teachdir", handleTeachDir);''')

# ---------- re-aim the saved direction by turning it with the rod ----------
# Before: only the part of the saved direction at right angles to the new
# rest was kept; a big change of rod angle (or slow tide drift over hours)
# shrank it to nothing and the direction was silently lost. Now the rest at
# the pull test is saved too, and the direction is turned by exactly the
# turn from that rest to the new rest.

rep('''Vec3 storedDir = { 0, 0, 0 };''', '''Vec3 storedRest = { 0, 0, 0 };   // rest position at that pull test (sensor frame)
bool haveStoredRest = false;
Vec3 storedDir = { 0, 0, 0 };''')

rep('''void refreshDirection()
{
  if(!haveStoredDir)
  {
    haveDir = false;
    return;
  }

  Vec3 d =
    vPerp(storedDir, restDir);''', '''// v turned by the turn that takes unit vector a to unit vector b
Vec3 vTurn(Vec3 v, Vec3 a, Vec3 b)
{
  Vec3 axis = vCross(a, b);
  float s = vLen(axis);
  float c = vDot(a, b);

  if(s < 1e-6)
    return v;

  Vec3 k = vScale(axis, 1.0 / s);

  return vAdd(
    vAdd(vScale(v, c), vScale(vCross(k, v), s)),
    vScale(k, vDot(k, v) * (1 - c))
  );
}

void refreshDirection()
{
  if(!haveStoredDir)
  {
    haveDir = false;
    return;
  }

  // turn the saved direction with the rod (from the rest at the pull test)
  Vec3 aimed =
    haveStoredRest ? vTurn(storedDir, storedRest, restDir) : storedDir;

  Vec3 d =
    vPerp(aimed, restDir);''')

rep('''  storedDir.x = prefs.getFloat("dx", 0);
  storedDir.y = prefs.getFloat("dy", 0);
  storedDir.z = prefs.getFloat("dz", 0);
''', '''  storedDir.x = prefs.getFloat("dx", 0);
  storedDir.y = prefs.getFloat("dy", 0);
  storedDir.z = prefs.getFloat("dz", 0);

  haveStoredRest = prefs.getBool("hasrest", false);
  storedRest.x = prefs.getFloat("rx", 0);
  storedRest.y = prefs.getFloat("ry", 0);
  storedRest.z = prefs.getFloat("rz", 0);

  if(vLen(storedRest) < 0.5)
    haveStoredRest = false;
''')

rep('''  prefs.putFloat("dz", storedDir.z);
''', '''  prefs.putFloat("dz", storedDir.z);

  prefs.putBool("hasrest", haveStoredRest);
  prefs.putFloat("rx", storedRest.x);
  prefs.putFloat("ry", storedRest.y);
  prefs.putFloat("rz", storedRest.z);
''')

rep('''    storedDir = vNorm(pullTestVec);
    haveStoredDir = true;''', '''    storedDir = vNorm(pullTestVec);
    haveStoredDir = true;
    storedRest = restDir;             // to turn the direction with the rod later
    haveStoredRest = true;''')

open(path, 'w').write(s)
print("ok", path)

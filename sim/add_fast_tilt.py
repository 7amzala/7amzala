#!/usr/bin/env python3
"""Rod sends a faster tilt ("ft", 0.1 s smoothing instead of 0.3 s) to the
watch, so the watch ring follows the rod tip without lag. Only for the
display: detection still uses the normal tilt.

    python3 add_fast_tilt.py <sketch.ino>      (edits in place)
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


rep('''const float GRAVITY_TAU = 0.30;          // s, rod angle smoothing
''', '''const float GRAVITY_TAU = 0.30;          // s, rod angle smoothing
const float FAST_TILT_TAU = 0.10;        // s, quicker angle for the watch ring (display only)
''')

rep('''float tiltDeg = 0;               // signed: + toward water, - away
''', '''float tiltDeg = 0;               // signed: + toward water, - away
Vec3 fastGravity = { 0, 0, 1 };
float fastTiltDeg = 0;           // same, less smoothing: the watch ring follows the tip
''')

rep('''    // No direction: fall back to "how much", like the old code
    tiltDeg = totalDeg;
    sideDeg = 0;
  }

  recentRest =''', '''    // No direction: fall back to "how much", like the old code
    tiltDeg = totalDeg;
    sideDeg = 0;
  }

  fastGravity =
    vAdd(
      fastGravity,
      vScale(
        vSub(rawAccel, fastGravity),
        alphaFor(FAST_TILT_TAU)
      )
    );

  if(vLen(fastGravity) > 0.5)
  {
    Vec3 fu = vNorm(fastGravity);

    if(haveDir)
      fastTiltDeg = atan2(vDot(fu, pullDir), vDot(fu, restDir)) * RAD_TO_DEGREES;
    else
      fastTiltDeg = vAngleDeg(fu, restDir);
  }

  recentRest =''')

rep('''  addField(json, "knockthr", knockThreshold, 4);
  addField(json, "tilt", tiltDeg, 2);
  addField(json, "allow"''', '''  addField(json, "knockthr", knockThreshold, 4);
  addField(json, "tilt", tiltDeg, 2);
  addField(json, "ft", fastTiltDeg, 2);
  addField(json, "allow"''')

open(path, 'w').write(s)
print("ok", path)

#!/usr/bin/env python3
"""A strong shake is no longer an instant SHARP STRIKE.

Short strong hit (<= KNOCK_MAX_MS): judged when it ends, as before
(onKnock -> SHARP STRIKE). Long strong shake: a fish only if the tip
also pulls toward the water (hit and run); otherwise it is a wind gust
and is ignored (shown on the dashboard).

    python3 add_gustguard.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()


def rep(old, new):
    global s
    n = s.count(old)
    if n != 1:
        sys.exit(f"{path}: expected 1 x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


rep('''bool sharpPending = false;''', '''bool sharpPending = false;

bool gustNoted = false;                 // this long shake was already reported as wind
unsigned long gustIgnoredAt = 0;        // last strong gust ignored (dashboard)''')

rep('''        knockTiltAtStart = tiltDeg;''', '''        knockTiltAtStart = tiltDeg;
        gustNoted = false;''')

rep('''// A very strong shake is a strike straight away, however long it lasts
bool checkViolentShake()
{
  if(!knockActive)
    return false;

  float level = profile().sharpAlone * knockThreshold;

  if(level > settings.sharpG)
    level = settings.sharpG;

  if(knockEnergy < level)
    return false;

  knockPeak = knockEnergy;
  startStrike("SHARP STRIKE");

  return true;
}''', '''// A strong shake. A short one (a hit) is judged when it ends, in
// onKnock(). A long one is a fish only if the tip also pulls toward
// the water (hit and run); otherwise it is a wind gust: a gust lasts,
// a hit does not.
bool checkViolentShake()
{
  if(!knockActive)
    return false;

  // Still short: wait until it ends (at most KNOCK_MAX_MS)
  if(millis() - knockStart <= KNOCK_MAX_MS)
    return false;

  float level = profile().sharpAlone * knockThreshold;

  if(level > settings.sharpG)
    level = settings.sharpG;

  if(knockPeak < level)
    return false;

  // In 0.6 s a wave moves the tip at most ~0.75 x its swing
  float needed = SHARP_PULL_MIN_DEG;

  if(waveSwing * 0.8 > needed)
    needed = waveSwing * 0.8;

  if(haveDir && tiltDeg - knockTiltAtStart >= needed)
  {
    startStrike("SHARP STRIKE");
    return true;
  }

  if(!gustNoted)
  {
    gustNoted = true;
    gustIgnoredAt = millis();

    Serial.println("STRONG LONG SHAKE, NO PULL: wind gust, ignored");
  }

  return false;
}''')

# ---------- dashboard ----------
rep('''  addField(json, "dropwatch", dropWatch);''', '''  addField(json, "dropwatch", dropWatch);
  addField(json, "gustago", gustIgnoredAt == 0 ? -1L : (long)((millis() - gustIgnoredAt) / 1000));''')

rep('''    if(d.dropwatch)
      sub = "Drop-back seen''', '''    if(d.gustago >= 0 && d.gustago < 5)
      sub = "Strong gust ignored (long shake, no pull toward the water).";
    else if(d.dropwatch)
      sub = "Drop-back seen''')

open(path, 'w').write(s)
print("ok", path)

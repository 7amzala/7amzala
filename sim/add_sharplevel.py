#!/usr/bin/env python3
"""The SHARP STRIKE level is never below 2.5 x the knock limit, so in strong
wind (knock limit above the fixed sharp level) ordinary knocks are not
all called SHARP STRIKE.   python3 add_sharplevel.py <sketch.ino>"""
import sys
path = sys.argv[1]; s = open(path).read()
def rep(old, new, count=1):
    global s
    if s.count(old) != count: sys.exit(f"{path}: expected {count} x {old[:60]!r}, found {s.count(old)}")
    s = s.replace(old, new)

rep('''void onKnock(bool settlingMode)
{''', '''// How strong a single hit must be for a SHARP STRIKE. Never below
// sharpWithPull x the knock limit: in strong wind the knock limit can
// rise above the fixed sharp level, and then every knock would count.
float sharpLevel()
{
  const SeaProfile& p = profile();

  float level = p.sharpAlone * knockThreshold;

  if(level > settings.sharpG)
    level = settings.sharpG;

  if(level < p.sharpWithPull * knockThreshold)
    level = p.sharpWithPull * knockThreshold;

  return level;
}

void onKnock(bool settlingMode)
{''')
rep('''    if(lastKnockStrength >= p.sharpAlone || knockPeak >= settings.sharpG)''',
    '''    if(knockPeak >= sharpLevel())''')
rep('''  if(lastKnockStrength >= p.sharpAlone || knockPeak >= settings.sharpG)''',
    '''  if(knockPeak >= sharpLevel())''')
rep('''  float level = profile().sharpAlone * knockThreshold;

  if(level > settings.sharpG)
    level = settings.sharpG;

  if(knockPeak < level)
    return false;''', '''  if(knockPeak < sharpLevel())
    return false;''')
open(path, 'w').write(s); print("ok", path)

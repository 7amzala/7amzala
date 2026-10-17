#!/usr/bin/env python3
"""Drop-back alone is no longer an alarm: it is shown, and the rod watches
for a pull forward. DROP-BACK + PULL is the alarm.

    python3 add_dropwatch.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()


def rep(old, new, optional=False):
    global s
    n = s.count(old)
    if optional and n == 0:
        return
    if n != 1:
        sys.exit(f"{path}: expected 1 x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


# ---------- state ----------
rep('''// How full the waves alone get the bucket.''', '''// Drop-back watch: a drop-back alone is normal (waves loosen the braid).
// Back and then forward past the normal position is the fish pattern.
const unsigned long DROP_WATCH_MS = 120000;   // watch this long for a pull after a drop-back
const unsigned long DROP_PULL_HOLD_MS = 500;  // the pull forward must hold this long

bool dropWatch = false;
unsigned long dropSeenAt = 0;                 // last drop-back seen (0 = never)
unsigned long dropSeenID = 0;                 // counts drop-backs (dashboard)
unsigned long dropPullSince = 0;

// How full the waves alone get the bucket.''')

# ---------- monitoring: drop-back bucket only notes it ----------
rep('''  if(!haveDir || settings.dropOn < 0.5)
  {
    dropBucket = 0;
    dropEpisodeStart = 0;
    return;
  }''', '''  if(!haveDir)
  {
    dropBucket = 0;
    dropEpisodeStart = 0;
    return;
  }''')

rep('''    episodeLength(dropEpisodeStart) >= sustainNeed
  )
  {
    startStrike("DROP-BACK");
  }
}''', '''    episodeLength(dropEpisodeStart) >= sustainNeed
  )
  {
    noteDropBack(true);
  }
}

// ---------- Drop-back watch ----------

void noteDropBack(bool watch)
{
  if(dropWatch)
    return;

  // While settling the same slack movement can last a while: note it once
  if(!watch && dropSeenAt != 0 && millis() - dropSeenAt < 10000)
    return;

  dropSeenAt = millis();
  dropSeenID++;

  dropWatch = watch;
  dropPullSince = 0;

  Serial.println(watch ? "DROP-BACK SEEN (no alarm) - watching for a pull forward" : "DROP-BACK SEEN while settling (no alarm)");
}

void updateDropWatch()
{
  if(!dropWatch)
    return;

  // Forward past the normal position, further than the biggest wave
  float needed = pullAllow;

  if(waveSwing > needed)
    needed = waveSwing;

  needed += settings.pullDeg * 0.5;

  if(tiltDeg >= needed)
  {
    if(dropPullSince == 0)
      dropPullSince = millis();

    if(millis() - dropPullSince >= DROP_PULL_HOLD_MS && settings.dropOn > 0.5)
    {
      dropWatch = false;
      startStrike("DROP-BACK + PULL");
      return;
    }
  }
  else
  {
    dropPullSince = 0;
  }

  if(millis() - dropSeenAt >= DROP_WATCH_MS)
  {
    // Nothing followed: the slack line is the new normal position
    dropWatch = false;

    setRest(recentRest);

    pullBucket = 0;
    dropBucket = 0;
    pullEpisodePeak = 0;
    dropEpisodePeak = 0;
    pullEpisodeStart = 0;
    dropEpisodeStart = 0;

    Serial.println("DROP-BACK: no pull followed, new rod position accepted");
  }
}''')

rep('''  updatePullBuckets();

  if(strikeActive)
    return;
''', '''  updatePullBuckets();

  if(strikeActive)
    return;

  updateDropWatch();

  if(strikeActive)
    return;
''')

rep('''  bool quiet =
    !knockActive &&''', '''  bool quiet =
    !dropWatch &&
    !knockActive &&''')

# ---------- settling: drop-back only noted ----------
rep('''    if(!settlePullAway || settings.dropOn > 0.5)
    {
      startStrike(settlePullAway ? "DROP-BACK" : "SLOW PULL");
      return;
    }''', '''    if(settlePullAway)
    {
      // Line went slack: only noted, the new position is learned anyway
      noteDropBack(false);
      settlePullCandidate = false;
    }
    else
    {
      startStrike("SLOW PULL");
      return;
    }''')

rep('''void startSettling(bool fromReady)
{
  rodState = ST_SETTLING;
''', '''void startSettling(bool fromReady)
{
  rodState = ST_SETTLING;

  dropWatch = false;
''')

rep('''void startEnvironmentLearning()
{
  rodState = ST_LEARNING;
''', '''void startEnvironmentLearning()
{
  rodState = ST_LEARNING;

  dropWatch = false;
''')

rep('''    case ST_PAUSED:
      pullBucket = 0;''', '''    case ST_PAUSED:
      dropWatch = false;
      pullBucket = 0;''')

# forward declarations (used before their definition)
rep('''bool dropWatch = false;''', '''bool dropWatch = false;
void noteDropBack(bool watch);''')

# ---------- dashboard data ----------
rep('''  addField(json, "peaktilt", strikePeakTilt, 2);
''', '''  addField(json, "peaktilt", strikePeakTilt, 2);

  addField(json, "dropwatch", dropWatch);
  addField(json, "dropseen", dropSeenAt == 0 ? -1L : (long)((millis() - dropSeenAt) / 1000));
  addField(json, "dropid", (long)dropSeenID);
''')

# ---------- dashboard page ----------
rep('''  else if(activeAlarmType === "DROP-BACK") dropPattern();''',
    '''  else if(activeAlarmType.startsWith("DROP")) dropPattern();''')
rep('''  else if(type === "DROP-BACK") repeatTime = 1900;''',
    '''  else if(type.startsWith("DROP")) repeatTime = 1900;''')

rep('''    let sub = "Watching knocks, slow pulls and drop-backs.";

    if(d.dirage >= 0 && d.dirage < 6)''', '''    let sub = "Watching knocks, slow pulls and drop-backs.";

    if(d.dropwatch)
      sub = "Drop-back seen " + d.dropseen + " s ago (line slack) \\u2013 no alarm. Watching for a pull forward\\u2026";
    else if(d.dirage >= 0 && d.dirage < 6)''')

rep('''<div><div class="setTop"><span>Drop-back alarm</span></div><div class="setHelp">Tip springs back, line goes slack.</div></div>''',
    '''<div><div class="setTop"><span>Drop-back + pull alarm</span></div><div class="setHelp">Line goes slack, then the tip pulls forward (fish). A drop-back alone is only shown, no alarm.</div></div>''')

# ---------- LoRa version: strike code for the watch ----------
rep('''  if(type == "DROP-BACK") return STRIKE_DROPBACK;''',
    '''  if(type.startsWith("DROP")) return STRIKE_DROPBACK;   // DROP-BACK + PULL''', optional=True)

open(path, 'w').write(s)
print("ok", path)

#!/usr/bin/env python3
"""Adds the "Slow pull sensitivity" live setting (pmul), which scales the
allowed lean and the bucket together, so it works in waves too (the pull
angle setting is only a minimum, which waves usually exceed).

    python3 add_pullmul.py <sketch.ino>      (edits in place)
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


rep('''  float knockBeep;       // 1 = single knock beep on''',
    '''  float knockBeep;       // 1 = single knock beep on
  float pullMul;         // slow pull: allowed lean and bucket x this (above 1 = less sensitive)''')

rep('''  1       // knockBeep''', '''  1,      // knockBeep
  1.0     // pullMul''')

rep('''  { "kbeep", &UserSettings::knockBeep,    0,    1 }''',
    '''  { "kbeep", &UserSettings::knockBeep,    0,    1 },
  { "pmul",  &UserSettings::pullMul,      0.8,  3.0 }''')

rep('''  if(bucketSize < BUCKET_MIN)
    bucketSize = BUCKET_MIN;
''', '''  if(bucketSize < BUCKET_MIN)
    bucketSize = BUCKET_MIN;

  // Slow pull sensitivity: works in waves too (pull angle is only a minimum)
  pullAllow *= settings.pullMul;
  bucketSize *= settings.pullMul;
''')

# ---------- dashboard ----------
rep('''  for(const k of ["kmul", "pdeg", "ptime", "rcnt", "rwin", "sharp"])''',
    '''  for(const k of ["kmul", "pdeg", "ptime", "rcnt", "rwin", "sharp", "pmul"])''')

rep('''  el("v_ptime").innerText = st.ptime.toFixed(1) + " s";''',
    '''  el("v_ptime").innerText = st.ptime.toFixed(1) + " s";
  el("v_pmul").innerText = "\\u00D7" + st.pmul.toFixed(2);''')

rep('''<div class="setGroup">PULLS</div>''', '''<div class="setGroup">PULLS</div>

<div class="setRow">
<div class="setTop"><span>Slow pull sensitivity</span><strong id="v_pmul"></strong></div>
<input type="range" id="s_pmul" min="0.8" max="3" step="0.05" oninput="slide('pmul')">
<div class="setEnds"><span>more sensitive</span><span>stricter</span></div>
<div class="setHelp">How far and how long the tip must lean toward the water. Works in waves too (the waves set the normal lean, this multiplies it).</div>
</div>''')

open(path, 'w').write(s)
print("ok", path)

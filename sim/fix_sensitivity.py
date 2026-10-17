#!/usr/bin/env python3
"""Knock-limit slider also moves the minimum limit; ADXL355 default a bit
less sensitive; 3D view shows typical noise + peak (steadier number)."""
import sys
path = sys.argv[1]
s = open(path).read()
def rep(old, new):
    global s
    if s.count(old) != 1:
        sys.exit(f"{path}: expected 1 x {old[:60]!r}, found {s.count(old)}")
    s = s.replace(old, new)

rep('const float KNOCK_FLOOR_ADXL355 = 0.004; // g, same for the ~10x quieter ADXL355',
    'const float KNOCK_FLOOR_ADXL355 = 0.006; // g, same for the ~10x quieter ADXL355')
rep('    clampFloat(thr, KNOCK_FLOOR, KNOCK_CEIL);',
    '''    // The slider also moves the smallest limit, so it works
    // even when the sensor is so quiet that the minimum applies
    clampFloat(thr, KNOCK_FLOOR * settings.knockMul, KNOCK_CEIL);''')
rep('  { "kmul",  &UserSettings::knockMul,     0.5,  2.5 },',
    '  { "kmul",  &UserSettings::knockMul,     0.5,  4.0 },')
rep('<input type="range" id="s_kmul" min="0.5" max="2.5" step="0.05" oninput="slide(\'kmul\')">',
    '<input type="range" id="s_kmul" min="0.5" max="4" step="0.05" oninput="slide(\'kmul\')">')

# 3D view: typical noise (median, ignores short bumps) and the peak
rep('''function mg(g)''', '''// Typical size of the shaking (median): one bump does not change it
function noiseTypical()
{
  if(noiseCloud.length === 0)
    return 0;

  const m = noiseCloud.map(p => Math.sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2])).sort((a, b) => a - b);

  return m[Math.floor(m.length / 2)];
}

function noisePeak()
{
  let peak = 0;

  for(const p of noiseCloud)
    peak = Math.max(peak, Math.sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));

  return peak;
}

function mg(g)''')
rep('''  const rms = noiseRms();
''', '''  const rms = noiseTypical();
  const peak = noisePeak();
''')
rep('''  ctx.fillText("NOISE  " + mg(rms) + " rms", 12, 10);''',
    '''  ctx.fillText("NOISE  " + mg(rms) + " typical", 12, 10);''')
rep('''"x the noise" + (noiseSensor ? " · " + noiseSensor : ""), 12, 46);''',
    '''"x the noise · peak " + mg(peak) + (noiseSensor ? " · " + noiseSensor : ""), 12, 46);''')
open(path, 'w').write(s)
print("ok", path)

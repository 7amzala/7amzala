#!/usr/bin/env python3
"""LIVE ROD view, detailed design: shaded carbon rod with handle, reel and
guides (line runs through them), metal sand spike with cups, glowing tip
light, surf washing up the beach, clouds moving with the wind, moon, splash
where the line goes in on a strike, and a glass info panel with a bend gauge
and slow pull / drop-back bars.

    python3 add_rodview_v2.py <sketch.ino> <rod_scene.js>      (edits in place)
"""
import re
import sys

path, scene_path = sys.argv[1], sys.argv[2]
s = open(path).read()
scene = open(scene_path).read()


def rep(old, new):
    global s
    n = s.count(old)
    if n != 1:
        sys.exit(f"{path}: expected 1 x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


# Older phone browsers have no ctx.roundRect: use our own
scene = re.sub(r'ctx\.roundRect\(', 'roundBox(ctx, ', scene)
scene = scene.replace('''  ctx.beginPath();
  roundBox(ctx, ''', '''  roundBox(ctx, ''')
helper = '''// Rounded box path (ctx.roundRect is missing on older phones)
function roundBox(c, x, y, w, h, r)
{
  c.beginPath();
  c.moveTo(x + r, y);
  c.arcTo(x + w, y, x + w, y + h, r);
  c.arcTo(x + w, y + h, x, y + h, r);
  c.arcTo(x, y + h, x, y, r);
  c.arcTo(x, y, x + w, y, r);
  c.closePath();
}

'''

a = s.index('// Surf rod in a sand spike, on the beach at night.')
b = s.index('function draw3DView()', a)
s = s[:a] + helper + scene.rstrip() + '\n\n' + s[b:]

rep('''    waveswing: d ? d.waveswing : 0.5,
    period: d && d.period > 0 ? d.period : 6
  });''', '''    waveswing: d ? d.waveswing : 0.5,
    period: d && d.period > 0 ? d.period : 6,
    wind: d ? d.wind : 0,
    allow: d ? d.allow : 0,
    bucket: d ? d.bucket : 0,
    drop: d ? d.drop : 0
  });''')

open(path, 'w').write(s)
print("ok", path)

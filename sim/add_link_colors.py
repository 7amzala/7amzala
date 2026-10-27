#!/usr/bin/env python3
"""Rod dashboard (the app's live screen): the watch line in the link colours.

Blue bars = the watch is on Bluetooth, yellow = on LoRa, grey = not connected
(the same colours as on the watch). "Watch connected · via LoRa · 500 m".

    python3 add_link_colors.py <sketch.ino>      (edits in place)
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


rep('''  var lora = d.wlink == 'lora', bt = d.wlink == 'bt';
  var c = bt ? '#22d3ff' : lora ? '#ffba52' : '#ff4d5e';''', '''  var lora = d.wlink == 'lora', bt = d.wlink == 'bt';
  var c = bt ? '#22a8ff' : lora ? '#ffc928' : '#6b7790';     // blue Bluetooth, yellow LoRa, grey none''')

rep('''  via.textContent = bt ? 'Bluetooth' : lora ? 'LoRa \\u00b7 ' + (d.lchan > 0 ? '1 km' : '500 m') +
    (d.lqual !== undefined ? ' \\u00b7 ' + QUAL[d.lqual] : '') : 'not connected';''', '''  via.textContent = bt ? 'connected \\u00b7 via Bluetooth' : lora ? 'connected \\u00b7 via LoRa \\u00b7 ' + (d.lchan > 0 ? '1 km' : '500 m') +
    (d.lqual !== undefined && d.lqual >= 2 ? ' \\u00b7 ' + QUAL[d.lqual] : '') : 'not connected';''')

open(path, 'w').write(s)
print(f"{path}: watch line in the link colours")

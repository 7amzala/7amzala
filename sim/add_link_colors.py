#!/usr/bin/env python3
"""Rod dashboard (the app's live screen): the watch line in the link colours.

Bluetooth: the word blue, the bars green. LoRa: the word and the bars yellow.
Not connected: grey. The same for the phone's own link and the watch line,
and the watch's bars (green Bluetooth, yellow LoRa).

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
  var c = bt ? '#3ddc84' : lora ? '#ffc928' : '#6b7790';     // bars: green Bluetooth, yellow LoRa, grey none
  var word = bt ? '#22a8ff' : lora ? '#ffc928' : '#6b7790';  // the word: blue Bluetooth, yellow LoRa''')

rep('''  via.textContent = bt ? 'Bluetooth' : lora ? 'LoRa \\u00b7 ' + (d.lchan > 0 ? '1 km' : '500 m') +
    (d.lqual !== undefined ? ' \\u00b7 ' + QUAL[d.lqual] : '') : 'not connected';''', '''  via.innerHTML = bt ? 'connected &middot; <b style="color:#22a8ff">Bluetooth</b>' :
    lora ? 'connected &middot; <b style="color:#ffc928">LoRa</b> &middot; ' + (d.lchan > 0 ? '1 km' : '500 m') +
    (d.lqual !== undefined && d.lqual >= 2 ? ' &middot; ' + QUAL[d.lqual] : '') : 'not connected';''')

rep('''  via.style.color = c;''', '''  via.style.color = bt || lora ? '#eaf3ff' : word;''')

# the phone's own link to the rod: the word "Bluetooth" blue, the bars green
rep('''  var c = n >= 3 ? '#3ddc84' : n == 2 ? '#ffba52' : '#ff4d5e';
  var b = document.querySelectorAll('#sigBars i');''', '''  var c = '#3ddc84';                    // Bluetooth bars: green (how many = how strong)
  var b = document.querySelectorAll('#sigBars i');''')
rep('''  document.getElementById('linkName').style.color = ready ? '' : '#ffba52';''',
    '''  document.getElementById('linkName').style.color = ready ? '#22a8ff' : '#ffba52';''')

open(path, 'w').write(s)
print(f"{path}: watch line in the link colours")

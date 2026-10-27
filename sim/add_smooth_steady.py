#!/usr/bin/env python3
"""Rod: smoother live screen, steadier LoRa channel.

- The dashboard asks for new data 20 times a second (was 10): the own
  Bluetooth line per device has room for it.
- LoRa AUTO channel (LoRa sketch only): the watch answers every 5 s, so
  12 s without hearing it (2 lost answers) was too quick to decide it is
  gone and change channel - the watch then had to search. Now 30 s to go
  to long range, 60 s to try the fast channel again, and the watch is told
  first when it was heard in the last 15 s (was 6 s).

    python3 add_smooth_steady.py <sketch.ino>      (edits in place)
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


rep('''  await updateData();
  setTimeout(pollLoop, 100);''', '''  await updateData();
  setTimeout(pollLoop, 50);             // 20 times a second''')

if 'void autoChannel(unsigned long now)' in s:
    rep('''  if(watchLastSeen != 0 && millis() - watchLastSeen < 6000)''',
        '''  if(watchLastSeen != 0 && millis() - watchLastSeen < 15000)''')
    rep('''    if(weak || quiet > 12000)''', '''    if(weak || quiet > 30000)''')
    rep('''        Serial.println("[LoRa] the watch is silent for 12 s -> long range");''',
        '''        Serial.println("[LoRa] the watch is silent for 30 s -> long range");''')
    rep('''    if((strong && now - strongSince > 60000) || quiet > 30000)
    {
      Serial.println(quiet > 30000 ? "[LoRa] the watch is silent for 30 s -> try the fast channel"''',
        '''    if((strong && now - strongSince > 60000) || quiet > 60000)
    {
      Serial.println(quiet > 60000 ? "[LoRa] the watch is silent for 60 s -> try the fast channel"''')

open(path, 'w').write(s)
print(f"{path}: smoother screen" + (", steadier LoRa channel" if 'void autoChannel' in s else ""))

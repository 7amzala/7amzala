#!/usr/bin/env python3
"""Dashboard: NEW PULL DIRECTION button (next to RELEARN).

The pull test now runs only once (the direction is kept). If the sensor is
ever mounted differently, this button asks for one pull test ("/teachdir").

    python3 add_teachdir_button.py <sketch.ino>      (edits in place)
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


rep('''<button class="action relearn" onclick="relearn()">RELEARN</button>''',
    '''<button class="action relearn" onclick="relearn()">RELEARN</button>
<button class="action relearn" onclick="teachDir()">NEW PULL DIRECTION</button>''')

rep('''async function relearn()
{''', '''// Only needed when the sensor is mounted differently: one pull test
async function teachDir()
{
  if(!confirm("New pull direction?\\n\\nOnly needed if the sensor is mounted differently.\\nThe rod will ask you to pull the line gently once."))
    return;

  stopContinuousAlarm();
  await fetch("/teachdir");
}

async function relearn()
{''')

open(path, 'w').write(s)
print("ok", path)

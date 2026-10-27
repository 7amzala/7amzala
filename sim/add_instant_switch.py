#!/usr/bin/env python3
"""Rod (LoRa sketch): link and channel changes reach the watch at once.

- Changing the LoRa channel: the rod always tells the watch first (even when
  it has not heard the watch lately - its answers may not reach the rod while
  the rod's packets reach the watch), 3 times, then changes. Before, one
  "move" packet, only when the watch was heard: a missed packet left the
  watch searching on the old channel.
- Choosing Bluetooth only: the rod keeps its LoRa heartbeat going 10 s more,
  so the watch hears the new choice and goes to Bluetooth at once (before,
  the rod went quiet at once and the watch waited up to a minute).
- Bluetooth only: a channel change is made at once (nothing to tell).

    python3 add_instant_switch.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()

if 'void startMove(int ch)' not in s:
    print(f"{path}: no LoRa, nothing to do")
    sys.exit(0)


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"{path}: expected {count} x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


rep('''void setLinkMode(int v)
{
  if(v < 0 || v > 2)
    return;

  linkMode = v;
''', '''unsigned long loraQuietFrom = 0;        // Bluetooth only: LoRa quiet from then (after telling the watch)

void setLinkMode(int v)
{
  if(v < 0 || v > 2)
    return;

  if(v == 1 && linkMode != 1)
    loraQuietFrom = millis() + 10000;   // 10 s more heartbeats: the watch hears "Bluetooth only"

  linkMode = v;
''')

rep('''// tell the watch first (if it hears us), then change
void startMove(int ch)
{
  if(ch == curChan || moveQueued || moveAfterTx)
    return;

  if(watchLastSeen != 0 && millis() - watchLastSeen < 15000)
  {
    moveTo = ch;
    moveQueued = true;
  }
  else
  {
    applyChannel(ch);
  }
}''', '''// tell the watch first (3 times: one may be lost), then change. Always: its
// answers may not reach us while ours reach it.
void startMove(int ch)
{
  if(ch == curChan || moveQueued || moveAfterTx)
    return;

  if(linkMode == 1 && (long)(millis() - loraQuietFrom) >= 0)
  {
    applyChannel(ch);                // Bluetooth only: LoRa is quiet, nobody to tell
    return;
  }

  moveTo = ch;
  moveLeft = 3;
  moveQueued = true;
}''')

rep('''bool moveQueued = false;        // tell the watch, then change channel''',
    '''bool moveQueued = false;        // tell the watch, then change channel
int moveLeft = 0;               // "move" packets still to send''')

rep('''  if(linkMode == 1)
    return;                        // Bluetooth only: the rod's LoRa stays quiet
''', '''  if(linkMode == 1 && (long)(now - loraQuietFrom) >= 0)
  {
    if(moveQueued)                 // (a move asked just before: change now)
    {
      moveQueued = false;
      applyChannel(moveTo);
    }

    return;                        // Bluetooth only: the rod's LoRa stays quiet
  }
''')

rep('''    if(loraSend(&m, sizeof(m), true))
    {
      moveQueued = false;
      moveAfterTx = true;
    }''', '''    if(loraSend(&m, sizeof(m), true))
    {
      if(--moveLeft > 0)
      {
        loraBackoffUntil = now + 80;   // the next copy a moment later
      }
      else
      {
        moveQueued = false;
        moveAfterTx = true;            // the last one sent: change channel
      }
    }''')

# how many of the watch's packets the rod hears, every 30 s on the Serial
# Monitor: 0 means the watch's LoRa sending does not reach the rod
rep('''  watchLastSeen = millis();
  watchRssi = radio.getRSSI();
  watchSnr = radio.getSNR();
''', '''  watchLastSeen = millis();
  watchRssi = radio.getRSSI();
  watchSnr = radio.getSNR();
  watchHeardN++;
''')

rep('''void autoChannel(unsigned long now)
{''', '''void autoChannel(unsigned long now)
{
  static unsigned long saidAt = 0;

  if(now - saidAt >= 30000)
  {
    saidAt = now;
    Serial.printf("[LoRa] the watch: %d packets heard in 30 s%s\\n", watchHeardN,
                  watchHeardN == 0 ? " (none: its LoRa sending does not reach the rod)" : "");
    watchHeardN = 0;
  }
''')

rep('''int moveLeft = 0;               // "move" packets still to send''', '''int moveLeft = 0;               // "move" packets still to send
int watchHeardN = 0;            // the watch's packets heard (Serial Monitor, every 30 s)''')

open(path, 'w').write(s)
print(f"{path}: link + channel changes reach the watch at once")

#!/usr/bin/env python3
"""Rod Bluetooth: never stop being findable while there is room.

- Every 5 s, if fewer than 2 phone/watch are connected, start advertising
  again (does nothing if it is already on). Covers a missed restart.
- Prints "Bluetooth: advertising, n connected" every 5 s on Serial, so
  a watch that cannot find the rod can be checked from the rod side.

Run after add_watch_link.py.

    python3 add_ble_keepalive.py <sketch.ino>      (edits in place)
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


rep('''    // Keep advertising while there is room for another phone/watch
    if(advertiseAgain)
    {
      advertiseAgain = false;

      if(clients < MAX_CLIENTS)
        BLEDevice::startAdvertising();
    }
''', '''    // Keep advertising while there is room for another phone/watch
    if(advertiseAgain || millis() - lastAdvertiseCheck >= 5000)
    {
      if(!advertiseAgain)
        Serial.printf("Bluetooth: %s, %d connected\\n",
                      clients < MAX_CLIENTS ? "advertising" : "full (not advertising)", (int)clients);

      advertiseAgain = false;
      lastAdvertiseCheck = millis();

      if(clients < MAX_CLIENTS)
        BLEDevice::startAdvertising();
    }
''')

rep('''  unsigned long lastPiece = 0;
''', '''  unsigned long lastPiece = 0;
  unsigned long lastAdvertiseCheck = 0;
''')

open(path, 'w').write(s)
print("ok", path)

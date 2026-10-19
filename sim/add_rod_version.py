#!/usr/bin/env python3
"""Rod prints its version at start, and unknown requests on Serial,
so an old sketch on the rod is easy to spot.

    python3 add_rod_version.py <sketch.ino>      (edits in place)
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


rep('''void setup()
{
  Serial.begin(115200);

  delay(1000);
''', '''void setup()
{
  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("=== LAARIF STRIKESENSE ROD - version 2.1 (Bluetooth: phone + watch) ===");
''')

rep('''    send(404, "text/plain", "NOT FOUND");''', '''    Serial.printf("Bluetooth: unknown request %s\\n", path.c_str());
    send(404, "text/plain", "NOT FOUND");''')

open(path, 'w').write(s)
print("ok", path)

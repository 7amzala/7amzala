#!/usr/bin/env python3
"""Rod Bluetooth: one line per phone / watch (Bluedroid, ESP32-S3).

Each device gets only its own answers, in pieces of its own MTU, at the pace
its own link takes them (the stack says when a link is full). The live
message goes to each line. Before, everything went to both devices, in
pieces as small as the smaller MTU and as slow as the slower link: the
phone's live graph lagged and the watch timed out and reconnected.

Sending to one device failing 3 times in a row: back to send-to-all.

Replaces the whole BleServer class with sim/ble_lines_class.cpp.

    python3 add_ble_lines.py <sketch.ino>      (edits in place)
"""
import os
import sys

path = sys.argv[1]
s = open(path).read()
new = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ble_lines_class.cpp')).read()

a = s.find('static portMUX_TYPE bleLock = portMUX_INITIALIZER_UNLOCKED;')
b = s.find('BleServer server;\n')

if a < 0 or b < 0 or 'class BleServer' not in s[a:b]:
    sys.exit(f"{path}: BleServer class not found")

if 'BLE_LINES' in s:
    sys.exit(f"{path}: already applied")

s = s[:a] + new + s[b + len('BleServer server;\n'):]


def rep(old, new_):
    global s
    if s.count(old) != 1:
        sys.exit(f"{path}: expected 1 x {old[:70]!r}, found {s.count(old)}")
    s = s.replace(old, new_)


# paired devices with NimBLE too (newer ESP32 cores on the S3): before, the
# rod counted 0 there, so it never hid and "Forget my devices" did nothing
rep('''#if defined(CONFIG_BLUEDROID_ENABLED)
  return esp_ble_get_bond_device_num();
#else
  return 0;
#endif''', '''#if defined(CONFIG_BLUEDROID_ENABLED)
  return esp_ble_get_bond_device_num();
#elif defined(CONFIG_NIMBLE_ENABLED)
  int n = 0;
  ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n);
  return n;
#else
  return 0;
#endif''')

rep('''    free(list);
  }
#endif

  Serial.println("Bluetooth: all paired devices forgotten - pairing open");''', '''    free(list);
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  ble_store_clear();
#endif

  Serial.println("Bluetooth: all paired devices forgotten - pairing open");''')
open(path, 'w').write(s)
print(f"{path}: Bluetooth lines per device")

#!/usr/bin/env python3
"""Sensor cable up to 3 m: I2C at 100 kHz instead of 400 kHz.

A long cable slows the I2C edges; 100 kHz is the standard speed for that.
One ADXL355 reading (9 bytes) then takes ~1.1 ms, still well inside the
2.5 ms between readings. Short cables work the same.

    python3 set_i2c_long_cable.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()

n = s.count("Wire.setClock(400000);")
if n < 1:
    sys.exit(f"{path}: no Wire.setClock(400000)")

s = s.replace("Wire.setClock(400000);", "Wire.setClock(SENSOR_I2C_HZ);")

old = "const unsigned long OVERSAMPLE_US = 2500;"
if s.count(old) != 1:
    sys.exit(f"{path}: OVERSAMPLE_US not found once")

s = s.replace(old, """// Sensor cable up to ~3 m: 100 kHz (400 kHz only works with short wires).
// With a 3 m cable also fit 2.2k pull-ups: SDA -> 3V3 and SCL -> 3V3 at the box.
const uint32_t SENSOR_I2C_HZ = 100000;

""" + old)

open(path, 'w').write(s)
print("ok", path, n, "x setClock")

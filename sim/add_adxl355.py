#!/usr/bin/env python3
"""Adds ADXL355 support (auto-detected, LIS3DH still works) to a StrikeSense sketch.

    python3 add_adxl355.py <in.ino> <out.ino>

Used for both the main sketch and the version without LoRa, so the
sensor code is the same in both.
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"expected {count} x {old[:60]!r}, found {n}")
    s = s.replace(old, new)


# ---------- header ----------
rep('''LIS3DH myIMU(I2C_MODE, 0x19);''', '''// Sensor: ADXL355 (EVAL-ADXL355-PMDZ, I2C) or LIS3DH, found automatically.
// ADXL355 wiring: pin 1 SCL -> D5, pin 2 SDA -> D4, pin 5 GND, pin 6 3V3,
// on the sensor link pin 3 -> 12 and pin 4 -> 11.
LIS3DH myIMU(I2C_MODE, 0x19);''')

# ---------- per-sensor floors ----------
rep('''const float KNOCK_FLOOR = 0.010;         // g, threshold never goes below this''',
    '''const float KNOCK_FLOOR_LIS3DH = 0.010;  // g, threshold never goes below this (LIS3DH)
const float KNOCK_FLOOR_ADXL355 = 0.004; // g, same for the ~10x quieter ADXL355
float KNOCK_FLOOR = KNOCK_FLOOR_LIS3DH;  // set in setup() for the sensor found''')

rep('''const float SENSOR_FLOOR_DEFAULT = 0.015; // g, sensor's own noise until measured''',
    '''const float SENSOR_FLOOR_LIS3DH = 0.015; // g, sensor's own noise until measured
const float SENSOR_FLOOR_ADXL355 = 0.002;
float SENSOR_FLOOR_DEFAULT = SENSOR_FLOOR_LIS3DH; // set in setup() for the sensor found''')

# ---------- reading ----------
rep('''void readOnce(Vec3& out)
{
  out.x = myIMU.readFloatAccelX();
  out.y = myIMU.readFloatAccelY();
  out.z = myIMU.readFloatAccelZ();
}''', '''// ---------- Which sensor ----------

enum SensorType
{
  SENSOR_NONE,
  SENSOR_LIS3DH,
  SENSOR_ADXL355
};

SensorType sensorType = SENSOR_NONE;

const char* sensorName()
{
  if(sensorType == SENSOR_ADXL355) return "ADXL355";
  if(sensorType == SENSOR_LIS3DH) return "LIS3DH";

  return "none";
}

// ---------- ADXL355 over I2C ----------

const uint8_t ADXL_DEVID_AD = 0x00;      // AD 1D ED
const uint8_t ADXL_XDATA3 = 0x08;        // X, Y, Z: 3 bytes each, 20 bits
const uint8_t ADXL_FILTER = 0x28;
const uint8_t ADXL_RANGE = 0x2C;
const uint8_t ADXL_POWER_CTL = 0x2D;
const uint8_t ADXL_RESET = 0x2F;
const float ADXL_LSB_PER_G = 256000.0;   // +-2 g range

uint8_t adxlAddr = 0;
unsigned long sensorReadErrors = 0;

bool i2cPresent(uint8_t addr)
{
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool adxlWrite(uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(adxlAddr);
  Wire.write(reg);
  Wire.write(value);

  return Wire.endTransmission() == 0;
}

bool adxlRead(uint8_t reg, uint8_t* buf, uint8_t n)
{
  Wire.beginTransmission(adxlAddr);
  Wire.write(reg);

  if(Wire.endTransmission(false) != 0)
    return false;

  if(Wire.requestFrom(adxlAddr, n) != n)
    return false;

  for(uint8_t i = 0; i < n; i++)
    buf[i] = Wire.read();

  return true;
}

float adxlAxis(const uint8_t* b)
{
  long v = ((long)b[0] << 12) | ((long)b[1] << 4) | (b[2] >> 4);

  if(v & 0x80000)
    v -= 0x100000;

  return v / ADXL_LSB_PER_G;
}

bool startADXL355()
{
  if(i2cPresent(0x53)) adxlAddr = 0x53;
  else if(i2cPresent(0x1D)) adxlAddr = 0x1D;
  else return false;

  uint8_t id[3] = { 0, 0, 0 };
  adxlRead(ADXL_DEVID_AD, id, 3);

  if(id[0] != 0xAD || id[1] != 0x1D || id[2] != 0xED)
  {
    Serial.printf("Device at 0x%02X is not an ADXL355 (ID %02X %02X %02X)\\n", adxlAddr, id[0], id[1], id[2]);
    return false;
  }

  adxlWrite(ADXL_RESET, 0x52);
  delay(50);

  adxlWrite(ADXL_RANGE, 0x01);       // +-2 g, the most sensitive
  adxlWrite(ADXL_FILTER, 0x03);      // 500 readings per second (filter 125 Hz)
  adxlWrite(ADXL_POWER_CTL, 0x00);   // start measuring

  delay(50);

  sensorType = SENSOR_ADXL355;
  sensorHighRes = true;              // always 20 bit

  Serial.printf("ADXL355 CONNECTED at 0x%02X\\n", adxlAddr);

  return true;
}

bool startLIS3DH()
{
  myIMU.settings.accelSampleRate = SENSOR_RATE;
  myIMU.settings.accelRange = 2;          // most sensitive range

  if(myIMU.begin() != 0)
    return false;

  // After begin(), which may reset the bus speed
  Wire.setClock(400000);

  sensorType = SENSOR_LIS3DH;

  Serial.println("LIS3DH CONNECTED");

  enableHighResolution();

  return true;
}

bool startSensor()
{
  if(startADXL355())
  {
    KNOCK_FLOOR = KNOCK_FLOOR_ADXL355;
    SENSOR_FLOOR_DEFAULT = SENSOR_FLOOR_ADXL355;
    return true;
  }

  if(startLIS3DH())
  {
    KNOCK_FLOOR = KNOCK_FLOOR_LIS3DH;
    SENSOR_FLOOR_DEFAULT = SENSOR_FLOOR_LIS3DH;
    return true;
  }

  return false;
}

// One reading in g. Returns false if the sensor did not answer.
bool readOnce(Vec3& out)
{
  if(sensorType == SENSOR_ADXL355)
  {
    uint8_t b[9];

    if(!adxlRead(ADXL_XDATA3, b, 9))
    {
      sensorReadErrors++;
      return false;
    }

    out.x = adxlAxis(b);
    out.y = adxlAxis(b + 3);
    out.z = adxlAxis(b + 6);

    return true;
  }

  out.x = myIMU.readFloatAccelX();
  out.y = myIMU.readFloatAccelY();
  out.z = myIMU.readFloatAccelZ();

  return true;
}''')

rep('''  Vec3 a;
  readOnce(a);

  oversampleSum = vAdd(oversampleSum, a);''', '''  Vec3 a;

  if(!readOnce(a))
    return;

  oversampleSum = vAdd(oversampleSum, a);''')

# readOnce uses sensorHighRes / enableHighResolution: move their declarations above
rep('''bool sensorHighRes = false;
uint8_t sensorReg1 = 0;
uint8_t sensorReg4 = 0;
''', '''bool sensorHighRes = false;
uint8_t sensorReg1 = 0;
uint8_t sensorReg4 = 0;

void enableHighResolution();
''')

# ---------- saved sensor noise: one value per sensor type ----------
rep('''  haveSensorFloor = !FORGET_SAVED_NOISE && prefs.getBool("hasfloor", false);
  savedSensorFloor = prefs.getFloat("floor", SENSOR_FLOOR_DEFAULT);''',
    '''  bool adxl = sensorType == SENSOR_ADXL355;

  haveSensorFloor = !FORGET_SAVED_NOISE && prefs.getBool(adxl ? "hasfloorA" : "hasfloor", false);
  savedSensorFloor = prefs.getFloat(adxl ? "floorA" : "floor", SENSOR_FLOOR_DEFAULT);''')

rep('''  prefs.putBool("hasfloor", true);
  prefs.putFloat("floor", sensorFloor);''', '''  bool adxl = sensorType == SENSOR_ADXL355;

  prefs.putBool(adxl ? "hasfloorA" : "hasfloor", true);
  prefs.putFloat(adxl ? "floorA" : "floor", sensorFloor);''')

# ---------- setup ----------
rep('''  Wire.begin();

  myIMU.settings.accelSampleRate = SENSOR_RATE;
  myIMU.settings.accelRange = 2;          // most sensitive range

  if(myIMU.begin() != 0)
  {
    Serial.println("LIS3DH NOT FOUND");

    while(1);
  }

  // After begin(), which may reset the bus speed
  Wire.setClock(400000);

  Serial.println("LIS3DH CONNECTED");

  enableHighResolution();
''', '''  Wire.begin();
  Wire.setClock(400000);

  // ADXL355 first, then LIS3DH. Keep trying until one answers.
  while(!startSensor())
  {
    Serial.println("NO SENSOR FOUND (ADXL355 or LIS3DH)");
    Serial.println("  ADXL355: pin 1 -> D5, pin 2 -> D4, pin 5 -> GND, pin 6 -> 3V3,");
    Serial.println("           links on the sensor: pin 3 -> 12, pin 4 -> 11");
    Serial.println("  Trying again in 2 s...");

    delay(2000);
  }

  sensorFloor = SENSOR_FLOOR_DEFAULT;
  knockThreshold = KNOCK_FLOOR;
''')

# ---------- dashboard ----------
rep('''  addField(json, "hr", sensorHighRes);''', '''  addField(json, "hr", sensorHighRes);

  json += ",\\"sname\\":\\"";
  json += sensorName();
  json += "\\"";

  addField(json, "serr", (long)sensorReadErrors);''')

rep('''(d.hr ? "high-res ✓" : "high-res OFF");''',
    '''d.sname + (d.hr ? " ✓" : " high-res OFF") + (d.serr > 0 ? " · " + d.serr + " read errors" : "");''')

open(dst, 'w').write(s)
print("ok", dst)

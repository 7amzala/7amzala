// =====================================================
// ADXL355 TEST (XIAO ESP32-S3 + EVAL-ADXL355-PMDZ)
//
// 1. Looks for every sensor on the I2C wires (SDA = D4, SCL = D5)
// 2. Checks the ADXL355 answers with its ID (AD 1D ED)
// 3. Prints X, Y, Z, total (should be 1.00 g lying still),
//    the sensor's own noise, and the temperature, once a second
//
// No library needed. Serial Monitor 115200.
// Arduino IDE: Tools -> USB CDC On Boot -> Enabled
// =====================================================

#include <Wire.h>

// ---------- ADXL355 registers ----------

#define REG_DEVID_AD   0x00   // always 0xAD
#define REG_DEVID_MST  0x01   // always 0x1D
#define REG_PARTID     0x02   // always 0xED
#define REG_STATUS     0x04
#define REG_TEMP2      0x06
#define REG_XDATA3     0x08   // X, Y, Z: 3 bytes each
#define REG_FILTER     0x28
#define REG_RANGE      0x2C
#define REG_POWER_CTL  0x2D
#define REG_RESET      0x2F

const float LSB_PER_G = 256000.0;   // +-2 g range

uint8_t adxlAddr = 0;

// ---------- I2C helpers ----------

bool present(uint8_t addr)
{
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool writeReg(uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(adxlAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readRegs(uint8_t reg, uint8_t* buf, uint8_t n)
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

// 20-bit value, left-aligned in 3 bytes
long toLong(const uint8_t* b)
{
  long v = ((long)b[0] << 12) | ((long)b[1] << 4) | (b[2] >> 4);

  if(v & 0x80000)
    v -= 0x100000;

  return v;
}

bool readAccel(float& x, float& y, float& z)
{
  uint8_t b[9];

  if(!readRegs(REG_XDATA3, b, 9))
    return false;

  x = toLong(b) / LSB_PER_G;
  y = toLong(b + 3) / LSB_PER_G;
  z = toLong(b + 6) / LSB_PER_G;

  return true;
}

float readTemperature()
{
  uint8_t b[2];

  if(!readRegs(REG_TEMP2, b, 2))
    return NAN;

  int raw = ((b[0] & 0x0F) << 8) | b[1];

  return 25.0 + (raw - 1885) / -9.05;
}

// ---------- Setup ----------

void scanBus()
{
  Serial.println("Looking for sensors on the wires...");

  int found = 0;

  for(uint8_t a = 1; a < 127; a++)
  {
    if(!present(a))
      continue;

    found++;
    Serial.printf("  found 0x%02X", a);

    if(a == 0x1D || a == 0x53) Serial.print("  <- ADXL355");
    if(a == 0x18 || a == 0x19) Serial.print("  <- LIS3DH");

    Serial.println();
  }

  if(found == 0)
  {
    Serial.println("  NOTHING FOUND. Check:");
    Serial.println("   - 3V3 and GND to the sensor");
    Serial.println("   - SDA (pin 2 of the sensor) to D4, SCL (pin 1) to D5");
    Serial.println("   - pin 4 (SCLK) linked to GND: without it the ADXL355 stays in SPI mode");
    Serial.println("   - the two 4.7k pull-up resistors (or keep the LIS3DH board connected)");
  }
}

void setup()
{
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("========== ADXL355 TEST ==========");

  Wire.begin();              // XIAO: SDA = D4, SCL = D5
  Wire.setClock(100000);     // slow and safe for the first test

  scanBus();

  if(present(0x1D)) adxlAddr = 0x1D;
  else if(present(0x53)) adxlAddr = 0x53;

  if(adxlAddr == 0)
  {
    Serial.println();
    Serial.println(">> No ADXL355 found. Fix the wiring and press RESET.");

    while(true)
      delay(1000);
  }

  uint8_t id[3] = { 0, 0, 0 };
  readRegs(REG_DEVID_AD, id, 3);

  Serial.printf("ADXL355 at 0x%02X, ID %02X %02X %02X", adxlAddr, id[0], id[1], id[2]);

  if(id[0] == 0xAD && id[1] == 0x1D && id[2] == 0xED)
    Serial.println("  -> CORRECT");
  else
    Serial.println("  -> WRONG (expected AD 1D ED): loose wire?");

  writeReg(REG_RESET, 0x52);   // fresh start
  delay(50);

  writeReg(REG_RANGE, 0x01);   // +-2 g (most sensitive), normal I2C speed
  writeReg(REG_FILTER, 0x03);  // 500 readings per second
  writeReg(REG_POWER_CTL, 0x00); // start measuring

  delay(100);

  Serial.println();
  Serial.println("Lay it flat and still. TOTAL should be about 1.000 g.");
  Serial.println("Then tap the table near it: NOISE jumps up.");
  Serial.println();
}

// ---------- Loop: 200 readings per second, print once a second ----------

float sum[3], sumSq[3];
int count = 0;
int errors = 0;
unsigned long lastRead = 0;
unsigned long lastPrint = 0;

void loop()
{
  if(micros() - lastRead < 5000)
    return;

  lastRead = micros();

  float a[3];

  if(!readAccel(a[0], a[1], a[2]))
  {
    errors++;
    return;
  }

  for(int k = 0; k < 3; k++)
  {
    sum[k] += a[k];
    sumSq[k] += a[k] * a[k];
  }

  count++;

  if(millis() - lastPrint < 1000 || count < 10)
    return;

  lastPrint = millis();

  float mean[3], noise = 0;

  for(int k = 0; k < 3; k++)
  {
    mean[k] = sum[k] / count;

    float var = sumSq[k] / count - mean[k] * mean[k];
    noise += var > 0 ? var : 0;

    sum[k] = sumSq[k] = 0;
  }

  noise = sqrt(noise);

  float total = sqrt(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);

  Serial.printf(
    "X %+.4f  Y %+.4f  Z %+.4f  | TOTAL %.4f g | NOISE %.5f g | %.1f C | %d readings, %d errors\n",
    mean[0], mean[1], mean[2], total, noise, readTemperature(), count, errors
  );

  count = 0;
  errors = 0;
}

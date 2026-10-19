// =====================================================
// WATCH BUZZ TEST (Pimoroni Haptic Bzzz, DRV2605L, I2C 0x5A)
//
// Only tests the buzzer: no screen, no Bluetooth.
// Wiring (watch header, right row):
//   Bzzz 2-5V -> 3V3   Bzzz GND -> GND   Bzzz SDA -> SDA   Bzzz SCL -> SCL
//
// Arduino IDE: Board "ESP32S3 Dev Module", USB CDC On Boot "Enabled".
// Serial Monitor 115200. Every 3 s: long buzz, then 3 short ones.
// =====================================================

#include <Wire.h>

const int SDA_PIN = 47;     // the header's SDA
const int SCL_PIN = 48;     // the header's SCL
const uint8_t BZZZ = 0x5A;

bool put(uint8_t reg, uint8_t v)
{
  Wire.beginTransmission(BZZZ);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

int get(uint8_t reg)
{
  Wire.beginTransmission(BZZZ);
  Wire.write(reg);

  if(Wire.endTransmission(false) != 0 || Wire.requestFrom(BZZZ, (uint8_t)1) != 1)
    return -1;

  return Wire.read();
}

void scan()
{
  Serial.print("I2C bus:");
  int n = 0;

  for(uint8_t a = 0x08; a < 0x78; a++)
  {
    Wire.beginTransmission(a);

    if(Wire.endTransmission() == 0)
    {
      Serial.printf(" 0x%02X%s", a, a == 0x5A ? "(BUZZER)" : a == 0x38 ? "(touch)" : "");
      n++;
    }
  }

  Serial.println(n ? "" : " nothing answers - check SDA/SCL wires");
}

bool setupBzzz()
{
  if(get(0x00) < 0)
  {
    Serial.println("BUZZER NOT FOUND at 0x5A - check: 2-5V->3V3, GND->GND, SDA->SDA, SCL->SCL");
    return false;
  }

  put(0x01, 0x80);                    // reset
  delay(100);

  for(int i = 0; i < 50 && (get(0x01) & 0x80); i++)
    delay(10);

  put(0x01, 0x00);                    // awake
  put(0x1A, 0x80 | (2 << 4) | (2 << 2) | 2);   // LRA motor
  put(0x03, 6);                       // LRA effects

  // auto-calibration (as Pimoroni's library)
  put(0x1E, 0x30);
  put(0x01, 0x07);
  put(0x0C, 1);

  uint32_t t = millis();
  while((get(0x0C) & 1) && millis() - t < 3000)
    delay(10);

  int st = get(0x00);
  Serial.printf("calibration %s (status 0x%02X)\n", (st & 0x08) ? "FAILED - using fixed drive" : "ok", st);

  if(st & 0x08)
    put(0x1D, get(0x1D) | 0x01);      // open loop

  put(0x01, 0x00);
  return true;
}

bool ready = false;

void setup()
{
  Serial.begin(115200);

  uint32_t t = millis();
  while(!Serial && millis() - t < 3000)
    delay(10);

  Serial.println("\n=== WATCH BUZZ TEST ===");
  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  scan();
  ready = setupBzzz();
}

void loop()
{
  if(!ready)
  {
    delay(3000);
    scan();
    ready = setupBzzz();
    return;
  }

  // 1: long buzz, full power (real-time mode)
  Serial.println("long buzz...");
  put(0x1D, get(0x1D) | 0x08);        // unsigned data
  put(0x01, 0x05);                    // real-time
  put(0x02, 255);
  delay(500);
  put(0x02, 0);
  put(0x01, 0x00);
  delay(400);

  // 2: three short buzzes (built-in effect 47)
  Serial.println("3 short buzzes...");
  put(0x04, 47);
  put(0x05, 0x80 | 15);
  put(0x06, 47);
  put(0x07, 0x80 | 15);
  put(0x08, 47);
  put(0x09, 0);
  put(0x0C, 1);

  delay(3000);
}

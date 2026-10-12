// =====================================================
// WATCH LoRa WIRING DIAGNOSTIC
// ESP32-S3-Touch-AMOLED-1.43 + SX1262 LoRa Node (HF)
//
// Checks each wire one by one and tells you which one is wrong.
// No library needed. Serial Monitor 115200.
// Arduino IDE: Tools -> USB CDC On Boot -> Enabled
// =====================================================

#include <SPI.h>

#define LORA_SCK     16   // CLK
#define LORA_MOSI    17
#define LORA_MISO    18
#define LORA_CS       5
#define LORA_RESET    6
#define LORA_BUSY     7
#define LORA_DIO1     1
#define LORA_RXEN     2
#define LORA_TXEN     3

SPIClass loraSPI(HSPI);

int problems = 0;

// Is something really driving this pin, or is the wire loose?
// A loose wire follows whatever pull-up / pull-down we switch on.
const char* pinState(int pin)
{
  pinMode(pin, INPUT_PULLUP);
  delay(3);
  int up = digitalRead(pin);

  pinMode(pin, INPUT_PULLDOWN);
  delay(3);
  int down = digitalRead(pin);

  pinMode(pin, INPUT);

  if(up == 1 && down == 0)
    return "FLOATING";

  return up ? "HIGH" : "LOW";
}

bool waitBusyLow(unsigned long ms)
{
  unsigned long start = millis();

  while(digitalRead(LORA_BUSY) == HIGH)
  {
    if(millis() - start > ms)
      return false;
  }

  return true;
}

// SX1262 "ReadRegister" command over SPI
void readRegisters(uint16_t addr, uint8_t* out, int len)
{
  waitBusyLow(100);

  loraSPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_CS, LOW);

  loraSPI.transfer(0x1D);
  loraSPI.transfer(addr >> 8);
  loraSPI.transfer(addr & 0xFF);
  loraSPI.transfer(0x00);           // status byte

  for(int i = 0; i < len; i++)
    out[i] = loraSPI.transfer(0x00);

  digitalWrite(LORA_CS, HIGH);
  loraSPI.endTransaction();
}

// Version text inside the chip, should start with "SX126"
bool readVersion(int mosi, int miso)
{
  loraSPI.end();
  loraSPI.begin(LORA_SCK, miso, mosi, -1);

  uint8_t v[16];
  readRegisters(0x0320, v, 16);

  char text[17];

  for(int i = 0; i < 16; i++)
    text[i] = (v[i] >= 32 && v[i] < 127) ? (char)v[i] : '.';

  text[16] = 0;

  Serial.printf("   MOSI=IO%d MISO=IO%d  ->  \"%s\"   (", mosi, miso, text);

  for(int i = 0; i < 6; i++)
    Serial.printf("%02X ", v[i]);

  Serial.println(")");

  return strncmp(text, "SX126", 5) == 0;
}

void setup()
{
  Serial.begin(115200);
  delay(2500);

  Serial.println();
  Serial.println("========== LoRa WIRING DIAGNOSTIC ==========");

  pinMode(LORA_CS, OUTPUT);
  digitalWrite(LORA_CS, HIGH);

  pinMode(LORA_RESET, OUTPUT);
  digitalWrite(LORA_RESET, HIGH);

  pinMode(LORA_RXEN, OUTPUT);
  digitalWrite(LORA_RXEN, LOW);

  pinMode(LORA_TXEN, OUTPUT);
  digitalWrite(LORA_TXEN, LOW);

  delay(50);

  // ---------- 1. BUSY wire + module power ----------

  Serial.println();
  Serial.println("1) BUSY wire / module power");

  const char* busy = pinState(LORA_BUSY);

  Serial.printf("   BUSY (IO%d) is %s\n", LORA_BUSY, busy);

  if(strcmp(busy, "FLOATING") == 0)
  {
    Serial.println("   >> PROBLEM: nothing drives BUSY.");
    Serial.println("      Either the module has NO POWER (check 3V3 and GND,");
    Serial.println("      measure 3.3V between 3V3 and GND pads on the module),");
    Serial.println("      or the BUSY wire is not on IO7 / not soldered.");
    problems++;
  }

  // ---------- 2. RESET wire ----------

  Serial.println();
  Serial.println("2) RESET wire");

  pinMode(LORA_BUSY, INPUT);

  digitalWrite(LORA_RESET, LOW);
  delay(2);
  int busyDuringReset = digitalRead(LORA_BUSY);
  digitalWrite(LORA_RESET, HIGH);

  unsigned long t0 = millis();
  bool ready = waitBusyLow(200);
  unsigned long readyMs = millis() - t0;

  Serial.printf("   BUSY during reset: %s (should be HIGH)\n", busyDuringReset ? "HIGH" : "LOW");

  if(ready)
    Serial.printf("   BUSY went LOW %lu ms after reset (should be < 20 ms)\n", readyMs);
  else
    Serial.println("   BUSY stayed HIGH after reset");

  if(!busyDuringReset)
  {
    Serial.println("   >> PROBLEM: the module did not react to RESET.");
    Serial.println("      Check the RESET wire is on IO6 and soldered.");
    problems++;
  }

  if(!ready)
  {
    Serial.println("   >> PROBLEM: module stuck busy. Check 3V3 power and the BUSY wire.");
    problems++;
  }

  // ---------- 3. MISO wire ----------

  Serial.println();
  Serial.println("3) MISO wire (module answers on it only while CS is LOW)");

  digitalWrite(LORA_CS, LOW);
  delay(1);
  const char* miso = pinState(LORA_MISO);
  digitalWrite(LORA_CS, HIGH);

  Serial.printf("   MISO (IO%d) with CS low is %s\n", LORA_MISO, miso);

  if(strcmp(miso, "FLOATING") == 0)
  {
    Serial.println("   >> Possible problem: MISO not driven. Check MISO on IO18");
    Serial.println("      and CS on IO5 (also try the swapped test below).");
  }

  // ---------- 4. Talk to the chip ----------

  Serial.println();
  Serial.println("4) Reading the chip name over SPI");

  bool ok = readVersion(LORA_MOSI, LORA_MISO);

  if(ok)
  {
    Serial.println("   >> OK: the SX1262 answered. Wiring of CLK, MOSI, MISO, CS is GOOD.");
  }
  else
  {
    Serial.println("   No answer. Trying with MOSI and MISO swapped...");

    bool swapped = readVersion(LORA_MISO, LORA_MOSI);

    if(swapped)
    {
      Serial.println("   >> FOUND IT: MOSI and MISO wires are SWAPPED.");
      Serial.println("      Swap the two wires (or tell Claude and the pins will be swapped in code).");
    }
    else
    {
      Serial.println("   >> PROBLEM: still no answer. Check CLK (IO16), CS (IO5),");
      Serial.println("      MOSI (IO17), MISO (IO18) and their solder joints.");
      Serial.println("      All FF = MISO stuck high / module off. All 00 = MISO stuck low or CLK missing.");
    }

    problems++;
  }

  // ---------- Result ----------

  Serial.println();
  Serial.println("============================================");

  if(problems == 0)
    Serial.println("ALL CHECKS PASSED - run watch_lora_test again.");
  else
    Serial.printf("%d problem(s) found - fix the ones marked >> above.\n", problems);

  Serial.println("Send the full text above to Claude.");
  Serial.println("============================================");
}

void loop()
{
}

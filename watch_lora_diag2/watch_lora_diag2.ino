// =====================================================
// WATCH LoRa WIRING DIAGNOSTIC v2
// ESP32-S3-Touch-AMOLED-1.43 + SX1262 LoRa Node (HF)
//
// Power, BUSY and RESET are already OK. This finds which of the
// 4 SPI wires (CLK, MOSI, MISO, CS) is wrong or mixed up.
// No library needed. Serial Monitor 115200.
// =====================================================

#define LORA_RESET    6
#define LORA_BUSY     7

// The 4 pins the SPI wires should be on
const int spiPins[4] = { 16, 17, 18, 5 };   // CLK, MOSI, MISO, CS (planned)
const char* roleName[4] = { "CLK", "MOSI", "MISO", "CS" };

int pSCK, pMOSI, pMISO, pCS;

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

void resetModule()
{
  digitalWrite(LORA_RESET, LOW);
  delay(2);
  digitalWrite(LORA_RESET, HIGH);

  unsigned long t = millis();

  while(digitalRead(LORA_BUSY) == HIGH && millis() - t < 100)
    ;
}

// Slow software SPI (mode 0) on any pins
uint8_t bbTransfer(uint8_t out)
{
  uint8_t in = 0;

  for(int i = 7; i >= 0; i--)
  {
    digitalWrite(pMOSI, (out >> i) & 1);
    delayMicroseconds(10);

    digitalWrite(pSCK, HIGH);
    delayMicroseconds(10);

    in |= digitalRead(pMISO) << i;

    digitalWrite(pSCK, LOW);
    delayMicroseconds(10);
  }

  return in;
}

void setRoles(int sck, int mosi, int miso, int cs)
{
  pSCK = sck;
  pMOSI = mosi;
  pMISO = miso;
  pCS = cs;

  pinMode(pCS, OUTPUT);
  digitalWrite(pCS, HIGH);

  pinMode(pSCK, OUTPUT);
  digitalWrite(pSCK, LOW);

  pinMode(pMOSI, OUTPUT);
  digitalWrite(pMOSI, LOW);

  pinMode(pMISO, INPUT);
}

void releasePins()
{
  for(int i = 0; i < 4; i++)
    pinMode(spiPins[i], INPUT);
}

// Reads the chip name, should start with "SX126"
bool readVersion(char* text)
{
  resetModule();

  digitalWrite(pCS, LOW);
  delayMicroseconds(20);

  bbTransfer(0x1D);       // ReadRegister
  bbTransfer(0x03);
  bbTransfer(0x20);       // address 0x0320
  bbTransfer(0x00);       // status

  for(int i = 0; i < 16; i++)
  {
    uint8_t c = bbTransfer(0x00);
    text[i] = (c >= 32 && c < 127) ? (char)c : '.';
  }

  text[16] = 0;

  digitalWrite(pCS, HIGH);

  return strncmp(text, "SX126", 5) == 0;
}

void setup()
{
  Serial.begin(115200);
  delay(2500);

  Serial.println();
  Serial.println("========== LoRa SPI DIAGNOSTIC v2 ==========");

  pinMode(LORA_RESET, OUTPUT);
  digitalWrite(LORA_RESET, HIGH);
  pinMode(LORA_BUSY, INPUT);

  // Also keep the antenna switch pins quiet
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);
  pinMode(3, OUTPUT);
  digitalWrite(3, LOW);

  // ---------- A. Which pin behaves like MISO? ----------
  // MISO is the only one the module drives, and only while CS is LOW.

  Serial.println();
  Serial.println("A) What each SPI pin looks like (all pins released):");

  releasePins();

  for(int i = 0; i < 4; i++)
    Serial.printf("   IO%-2d (planned %-4s): %s\n", spiPins[i], roleName[i], pinState(spiPins[i]));

  // ---------- B. Planned wiring with slow software SPI ----------

  Serial.println();
  Serial.println("B) Planned wiring, slow software SPI:");

  char text[17];

  setRoles(16, 17, 18, 5);
  bool plannedOk = readVersion(text);

  Serial.printf("   CLK=IO16 MOSI=IO17 MISO=IO18 CS=IO5  ->  \"%s\"\n", text);

  releasePins();

  if(plannedOk)
  {
    Serial.println();
    Serial.println(">> The wiring is CORRECT. The chip answers with slow SPI.");
    Serial.println("   The problem is the SPI speed or bus setting: tell Claude.");
    Serial.println("============================================");
    return;
  }

  // ---------- C. Try every way the 4 wires could be mixed up ----------

  Serial.println();
  Serial.println("C) Trying all 24 ways the 4 SPI wires could be mixed up...");

  int idx[4] = { 0, 1, 2, 3 };
  bool found = false;

  // all permutations of 4
  for(int a = 0; a < 4 && !found; a++)
  for(int b = 0; b < 4 && !found; b++)
  for(int c = 0; c < 4 && !found; c++)
  for(int d = 0; d < 4 && !found; d++)
  {
    if(a == b || a == c || a == d || b == c || b == d || c == d)
      continue;

    setRoles(spiPins[a], spiPins[b], spiPins[c], spiPins[d]);

    bool ok = readVersion(text);

    releasePins();

    if(ok)
    {
      found = true;

      Serial.println();
      Serial.printf("   FOUND: chip answered \"%s\"\n", text);
      Serial.println();
      Serial.println(">> The wires are MIXED UP. They are really connected like this:");
      Serial.printf("   LoRa CLK  is on IO%d\n", spiPins[a]);
      Serial.printf("   LoRa MOSI is on IO%d\n", spiPins[b]);
      Serial.printf("   LoRa MISO is on IO%d\n", spiPins[c]);
      Serial.printf("   LoRa CS   is on IO%d\n", spiPins[d]);
      Serial.println("   Send this to Claude: the pins can simply be changed in the code.");
    }
  }

  if(!found)
  {
    Serial.println();
    Serial.println(">> No combination worked.");
    Serial.println("   At least one SPI wire is not connected (bad solder joint,");
    Serial.println("   or on a different header pin than IO16 / IO17 / IO18 / IO5).");
    Serial.println("   Check with a multimeter (beep mode, power OFF):");
    Serial.println("     LoRa CLK  pad <-> watch header IO16");
    Serial.println("     LoRa MOSI pad <-> watch header IO17");
    Serial.println("     LoRa MISO pad <-> watch header IO18");
    Serial.println("     LoRa CS   pad <-> watch header IO5");
  }

  Serial.println("============================================");
  Serial.println("Send the full text above to Claude.");
}

void loop()
{
}

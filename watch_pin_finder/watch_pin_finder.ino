// =====================================================
// WATCH PIN FINDER
// ESP32-S3-Touch-AMOLED-1.43
//
// Touch a header pin with the probe wire and the Serial Monitor
// tells you which IO it is. No library needed.
//
// PROBE: a wire from the watch's GND, with a resistor in it
// (any value from 220 ohm to 4.7k ohm). The resistor makes it safe
// even if you touch a 3V3 pin by mistake.
//
// Do this BEFORE the LoRa module is soldered on (or with it
// unplugged), otherwise the module holds some pins.
//
// Serial Monitor 115200. Tools -> USB CDC On Boot -> Enabled
// =====================================================

// The free pins on this board (Waveshare GPIO table), plus the
// UART pins, which are free when the USB port is used for Serial
const int PINS[] = { 1, 2, 3, 5, 6, 7, 16, 17, 18, 45, 46, 43, 44 };
const int COUNT = sizeof(PINS) / sizeof(PINS[0]);

// What each pin is for in the StrikeSense LoRa wiring
const char* job(int pin)
{
  switch(pin)
  {
    case 5:  return "LoRa CS";
    case 16: return "LoRa CLK";
    case 17: return "LoRa MOSI";
    case 18: return "LoRa MISO";
    case 6:  return "LoRa RESET";
    case 7:  return "LoRa BUSY";
    case 1:  return "LoRa DIO1";
    case 2:  return "LoRa RXEN";
    case 3:  return "LoRa TXEN";
    case 43: return "TXD (not used)";
    case 44: return "RXD (not used)";
    default: return "not used";
  }
}

bool wasLow[64];

void setup()
{
  Serial.begin(115200);
  delay(2000);

  for(int i = 0; i < COUNT; i++)
  {
    pinMode(PINS[i], INPUT_PULLUP);
    wasLow[PINS[i]] = false;
  }

  delay(50);

  Serial.println();
  Serial.println("========== WATCH PIN FINDER ==========");
  Serial.println("Touch a header pin with the GND probe (through the resistor).");
  Serial.println();

  // A pin already LOW at start is held by something (LoRa module
  // still connected, solder bridge, wire touching GND)
  for(int i = 0; i < COUNT; i++)
  {
    if(digitalRead(PINS[i]) == LOW)
    {
      Serial.printf("  NOTE: IO%d is already LOW without touching: something holds it to GND\n", PINS[i]);
      wasLow[PINS[i]] = true;
    }
  }
}

void loop()
{
  for(int i = 0; i < COUNT; i++)
  {
    int pin = PINS[i];
    bool low = digitalRead(pin) == LOW;

    if(low && !wasLow[pin])
      Serial.printf(">>> You are touching IO%d   (%s)\n", pin, job(pin));

    wasLow[pin] = low;
  }

  delay(20);
}

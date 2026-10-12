// =====================================================
// WATCH LoRa TEST - ESP32-S3-Touch-AMOLED-1.43
//                 + SX1262 LoRa Node (HF)
//
// Only listens for "PING" messages from the rod and prints
// them in the Serial Monitor (115200). No screen code.
// Library: RadioLib.
// Arduino IDE: Tools -> USB CDC On Boot -> Enabled
// =====================================================

#include <SPI.h>
#include <RadioLib.h>

// ---------- Wiring: LoRa module -> watch header ----------

#define LORA_SCK     16   // CLK
#define LORA_MOSI    17
#define LORA_MISO    18
#define LORA_CS       5
#define LORA_RESET    6
#define LORA_BUSY     7
#define LORA_DIO1     1
#define LORA_RXEN     2
#define LORA_TXEN     3

// ---------- Radio settings: MUST be the same on the rod ----------

#define LORA_FREQ_MHZ   868.0   // 868 Europe / Africa, 915 Americas / Australia
#define LORA_BW_KHZ     125.0
#define LORA_SF         9
#define LORA_CR         7
#define LORA_SYNC_WORD  0x12
#define LORA_POWER_DBM  14
#define LORA_PREAMBLE   8

// Own SPI bus (SPI3), so it never conflicts with the display later
SPIClass loraSPI(HSPI);

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RESET, LORA_BUSY, loraSPI,
                          SPISettings(1000000, MSBFIRST, SPI_MODE0));   // 1 MHz: safe with hand-soldered wires

volatile bool packetFlag = false;

void IRAM_ATTR onPacket()
{
  packetFlag = true;
}

int16_t startRadio(float tcxoVoltage)
{
  return radio.begin(
    LORA_FREQ_MHZ,
    LORA_BW_KHZ,
    LORA_SF,
    LORA_CR,
    LORA_SYNC_WORD,
    LORA_POWER_DBM,
    LORA_PREAMBLE,
    tcxoVoltage
  );
}

void setup()
{
  Serial.begin(115200);
  delay(2000);

  loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

  Serial.println();
  Serial.println("[WATCH] starting SX1262...");

  // Clock is a TCXO or a crystal: try the TCXO first, then the crystal
  int16_t state = startRadio(1.8);

  if(state != RADIOLIB_ERR_NONE)
  {
    Serial.printf("[WATCH] with TCXO 1.8V: error %d, trying crystal\n", state);
    state = startRadio(0);
  }

  if(state != RADIOLIB_ERR_NONE)
  {
    Serial.printf("[WATCH] LoRa FAILED, error %d\n", state);

    if(state == RADIOLIB_ERR_CHIP_NOT_FOUND)
      Serial.println("[WATCH] Chip not found: check 3V3, GND, CLK, MOSI, MISO, CS, RESET, BUSY");

    while(true)
      delay(1000);
  }

  // This module has its own antenna switch on RXEN / TXEN
  radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);

  radio.setPacketReceivedAction(onPacket);

  state = radio.startReceive();

  if(state != RADIOLIB_ERR_NONE)
  {
    Serial.printf("[WATCH] startReceive failed, error %d\n", state);

    while(true)
      delay(1000);
  }

  Serial.printf(
    "[WATCH] LoRa OK  %.1f MHz  SF%d  - waiting for the rod...\n",
    LORA_FREQ_MHZ, LORA_SF
  );
}

void loop()
{
  if(!packetFlag)
    return;

  packetFlag = false;

  String text;
  int16_t state = radio.readData(text);

  if(state == RADIOLIB_ERR_NONE)
  {
    Serial.printf(
      "[WATCH] received \"%s\"   RSSI %.0f dBm   SNR %.1f dB\n",
      text.c_str(),
      radio.getRSSI(),
      radio.getSNR()
    );
  }
  else if(state == RADIOLIB_ERR_CRC_MISMATCH)
  {
    Serial.println("[WATCH] damaged packet (CRC error)");
  }
  else
  {
    Serial.printf("[WATCH] read failed, error %d\n", state);
  }

  radio.startReceive();
}

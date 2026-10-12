// =====================================================
// ROD LoRa TEST - Seeed XIAO ESP32-S3 + Wio-SX1262
//
// Sends "PING <n>" every 5 seconds so the watch can show
// that it receives the rod. Library: RadioLib.
// =====================================================

#include <SPI.h>
#include <RadioLib.h>

// ---------- XIAO ESP32-S3 + Wio-SX1262 kit pins ----------

#define LORA_SCK      7
#define LORA_MISO     8
#define LORA_MOSI     9
#define LORA_CS      41
#define LORA_RESET   42
#define LORA_BUSY    40
#define LORA_DIO1    39

// ---------- Radio settings: MUST be the same on the watch ----------

#define LORA_FREQ_MHZ   868.0   // 868 Europe / Africa, 915 Americas / Australia
#define LORA_BW_KHZ     125.0
#define LORA_SF         9
#define LORA_CR         7       // coding rate 4/7
#define LORA_SYNC_WORD  0x12    // private network
#define LORA_POWER_DBM  14
#define LORA_PREAMBLE   8

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RESET, LORA_BUSY);

unsigned long pingCount = 0;
unsigned long lastPing = 0;

void setup()
{
  Serial.begin(115200);
  delay(1500);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

  Serial.println();
  Serial.println("[LoRa] starting Wio-SX1262...");

  // The Wio-SX1262 uses a 1.8V TCXO powered from DIO3
  int16_t state = radio.begin(
    LORA_FREQ_MHZ,
    LORA_BW_KHZ,
    LORA_SF,
    LORA_CR,
    LORA_SYNC_WORD,
    LORA_POWER_DBM,
    LORA_PREAMBLE,
    1.8
  );

  if(state != RADIOLIB_ERR_NONE)
  {
    Serial.printf("[LoRa] FAILED, error %d\n", state);

    while(true)
      delay(1000);
  }

  // The Wio-SX1262 antenna switch is driven by DIO2
  radio.setDio2AsRfSwitch(true);

  Serial.printf(
    "[LoRa] OK  %.1f MHz  BW %.0f kHz  SF%d  CR 4/%d  sync 0x%02X\n",
    LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR, LORA_SYNC_WORD
  );
}

void loop()
{
  if(millis() - lastPing < 5000)
    return;

  lastPing = millis();
  pingCount++;

  String msg = "PING " + String(pingCount);

  int16_t state = radio.transmit(msg);

  if(state == RADIOLIB_ERR_NONE)
    Serial.printf("[LoRa] sent \"%s\"\n", msg.c_str());
  else
    Serial.printf("[LoRa] send failed, error %d\n", state);
}

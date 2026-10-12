// =====================================================
// LoRa link for the StrikeSense watch (RadioLib)
// =====================================================

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

#include "lora_link.h"

// ---------- Wiring: SX1262 LoRa Node (HF) -> watch header ----------

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
#define LORA_CR         7       // coding rate 4/7
#define LORA_SYNC_WORD  0x12    // private network
#define LORA_POWER_DBM  14
#define LORA_PREAMBLE   8

// The display already uses SPI2 (FSPI). LoRa gets its own bus: SPI3 (HSPI).
static SPIClass loraSPI(HSPI);

static SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RESET, LORA_BUSY, loraSPI);

static volatile bool packetFlag = false;

static lora_link_status_t status = { false, 0, 0, 0, 0.0f, 0.0f };

static void IRAM_ATTR onPacket(void)
{
  packetFlag = true;
}

static int16_t startRadio(float tcxoVoltage)
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

bool lora_link_begin(void)
{
  loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

  Serial.println();
  Serial.println("[LoRa] starting SX1262...");

  // The module clock is either a TCXO (powered from DIO3) or a plain
  // crystal. Try the TCXO first, then the crystal.
  int16_t state = startRadio(1.8);

  if(state != RADIOLIB_ERR_NONE)
  {
    Serial.printf("[LoRa] with TCXO 1.8V: error %d, trying crystal\n", state);
    state = startRadio(0);
  }

  status.init_error = state;

  if(state != RADIOLIB_ERR_NONE)
  {
    status.radio_ok = false;

    Serial.printf("[LoRa] FAILED, error %d\n", state);

    if(state == RADIOLIB_ERR_CHIP_NOT_FOUND)
    {
      Serial.println("[LoRa] Chip not found: check 3V3, GND, CLK, MOSI, MISO, CS, RESET, BUSY wiring");
    }

    return false;
  }

  // This module has its own antenna switch on RXEN / TXEN
  radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);

  radio.setPacketReceivedAction(onPacket);

  state = radio.startReceive();

  if(state != RADIOLIB_ERR_NONE)
  {
    status.radio_ok = false;
    status.init_error = state;

    Serial.printf("[LoRa] startReceive failed, error %d\n", state);
    return false;
  }

  status.radio_ok = true;

  Serial.printf(
    "[LoRa] OK  %.1f MHz  BW %.0f kHz  SF%d  CR 4/%d  sync 0x%02X\n",
    LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR, LORA_SYNC_WORD
  );
  Serial.println("[LoRa] listening for the rod...");

  return true;
}

void lora_link_poll(void)
{
  if(!status.radio_ok || !packetFlag)
    return;

  packetFlag = false;

  String text;
  int16_t state = radio.readData(text);

  if(state == RADIOLIB_ERR_NONE)
  {
    status.packets++;
    status.last_rx_ms = millis();
    status.last_rssi = radio.getRSSI();
    status.last_snr = radio.getSNR();

    Serial.printf(
      "[LoRa] #%lu  \"%s\"  RSSI %.0f dBm  SNR %.1f dB\n",
      (unsigned long)status.packets,
      text.c_str(),
      status.last_rssi,
      status.last_snr
    );
  }
  else if(state == RADIOLIB_ERR_CRC_MISMATCH)
  {
    Serial.println("[LoRa] damaged packet (CRC error)");
  }
  else
  {
    Serial.printf("[LoRa] read failed, error %d\n", state);
  }

  radio.startReceive();
}

lora_link_status_t lora_link_get_status(void)
{
  return status;
}

uint8_t lora_link_signal_bars(void)
{
  if(status.last_rx_ms == 0 || millis() - status.last_rx_ms > 6000)
    return 0;

  if(status.last_rssi > -80)  return 4;
  if(status.last_rssi > -95)  return 3;
  if(status.last_rssi > -105) return 2;

  return 1;
}

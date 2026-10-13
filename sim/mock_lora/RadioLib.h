// Virtual radio for the host simulator: every SX1262 created in the
// process shares one "air". Packets take their real airtime and can
// be lost on purpose (simLoraLoss) to test retries.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "SPI.h"

#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ERR_CHIP_NOT_FOUND (-2)
#define RADIOLIB_ERR_CRC_MISMATCH (-7)
#define RADIOLIB_NC 0xFFFFFFFF
#define RADIOLIB_DEFAULT_SPI_SETTINGS SPISettings(2000000, MSBFIRST, SPI_MODE0)
#define IRAM_ATTR

extern unsigned long simMillis;
inline float simLoraLoss = 0.0f;      // 0..1 chance a packet is lost
inline unsigned long simLoraSent = 0;
inline unsigned long simLoraLost = 0;

struct Module {
  Module(uint32_t, uint32_t, uint32_t, uint32_t = RADIOLIB_NC) {}
  Module(uint32_t, uint32_t, uint32_t, uint32_t, SPIClass&, SPISettings = RADIOLIB_DEFAULT_SPI_SETTINGS) {}
};

struct SX1262 {
  static std::vector<SX1262*>& all() { static std::vector<SX1262*> v; return v; }

  void (*cb)(void) = nullptr;
  bool listening = false;
  bool txing = false;
  unsigned long txDoneAt = 0;
  std::vector<uint8_t> txData, rxData;

  SX1262(Module*) { all().push_back(this); }

  int16_t begin(float = 434.0, float = 125.0, uint8_t = 9, uint8_t = 7, uint8_t = 0x12, int8_t = 10, uint16_t = 8, float = 1.6, bool = false) { return 0; }
  int16_t setDio2AsRfSwitch(bool = true) { return 0; }
  void setRfSwitchPins(uint32_t, uint32_t) {}
  void setDio1Action(void (*f)(void)) { cb = f; }
  int16_t startReceive() { listening = true; return 0; }
  int16_t finishTransmit() { return 0; }
  size_t getPacketLength(bool = true) { return rxData.size(); }
  int16_t readData(uint8_t* d, size_t len) { memcpy(d, rxData.data(), len); return 0; }
  float getRSSI() { return -60; }
  float getSNR() { return 9; }

  int16_t startTransmit(const uint8_t* d, size_t len, uint8_t = 0) {
    txData.assign(d, d + len);
    txing = true;
    listening = false;
    txDoneAt = simMillis + 30 + len * 2;   // ~SF7 airtime
    return 0;
  }

  // Called by the simulator every millisecond
  static void tickAir() {
    for (SX1262* s : all()) {
      if (!s->txing || simMillis < s->txDoneAt) continue;
      s->txing = false;
      simLoraSent++;
      bool lost = (rand() % 1000) < simLoraLoss * 1000;
      if (lost) simLoraLost++;
      for (SX1262* o : all()) {
        if (o == s || lost || !o->listening || o->txing) continue;
        o->rxData = s->txData;
        if (o->cb) o->cb();
      }
      if (s->cb) s->cb();   // "transmit done"
    }
  }
};

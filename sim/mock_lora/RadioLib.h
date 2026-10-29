// Virtual radio for the host simulator: every SX1262 created in the
// process shares one "air". Packets take their real LoRa airtime, reach
// only radios on the same frequency and bandwidth, get weaker with
// distance (each radio has a position x in metres) and can be lost on
// purpose (simLoraLoss) or by colliding with another packet.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include "SPI.h"

#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ERR_CHIP_NOT_FOUND (-2)
#define RADIOLIB_ERR_CRC_MISMATCH (-7)
#define RADIOLIB_CHANNEL_FREE (-702)
#define RADIOLIB_LORA_DETECTED (-15)
#define RADIOLIB_NC 0xFFFFFFFF
#define RADIOLIB_DEFAULT_SPI_SETTINGS SPISettings(2000000, MSBFIRST, SPI_MODE0)
#define IRAM_ATTR

extern unsigned long simMillis;
inline float simLoraLoss = 0.0f;      // 0..1 chance a packet is lost
class SX1262;
inline SX1262* simDeafTx = nullptr;   // this radio's packets never reach anyone (its sending is broken)
inline SX1262* simCadBusy = nullptr;  // this radio's "listen before talk" always says busy (noise)
inline unsigned long simLoraSent = 0;
inline unsigned long simLoraLost = 0;
inline unsigned long simLoraCollided = 0;

struct Module {
  Module(uint32_t, uint32_t, uint32_t, uint32_t = RADIOLIB_NC) {}
  Module(uint32_t, uint32_t, uint32_t, uint32_t, SPIClass&, SPISettings = RADIOLIB_DEFAULT_SPI_SETTINGS) {}
};

struct SX1262 {
  static std::vector<SX1262*>& all() { static std::vector<SX1262*> v; return v; }

  void (*cb)(void) = nullptr;
  bool listening = false;
  bool txing = false;
  unsigned long txStart = 0, txDoneAt = 0;
  std::vector<uint8_t> txData, rxData;
  float freq = 869.525f, bw = 125.0f, power = 14;
  int sf = 7;
  float x = 0;                        // position on the beach, metres
  float lastRssi = -60, lastSnr = 9;

  SX1262(Module*) { all().push_back(this); }

  int16_t begin(float f = 434.0, float b = 125.0, uint8_t s = 9, uint8_t = 7, uint8_t = 0x12, int8_t p = 10, uint16_t = 8, float = 1.6, bool = false)
  { freq = f; bw = b; sf = s; power = p; return 0; }
  int16_t setFrequency(float f) { freq = f; return 0; }
  int16_t setBandwidth(float b) { bw = b; return 0; }
  int16_t setSpreadingFactor(uint8_t s) { sf = s; return 0; }
  int16_t setOutputPower(int8_t p) { power = p; return 0; }
  int16_t setDio2AsRfSwitch(bool = true) { return 0; }
  void setRfSwitchPins(uint32_t, uint32_t) {}
  void setDio1Action(void (*f)(void)) { cb = f; }
  int16_t startReceive() { listening = true; return 0; }
  int16_t standby() { listening = false; return 0; }
  int16_t finishTransmit() { return 0; }
  size_t getPacketLength(bool = true) { return rxData.size(); }
  // reading a packet takes a few ms on the real chip (SPI): the clock moves on
  int16_t readData(uint8_t* d, size_t len) { memcpy(d, rxData.data(), len); simMillis += 3; return 0; }
  float getRSSI() { return lastRssi; }
  float getSNR() { return lastSnr; }

  bool sameChannel(const SX1262* o) const { return fabsf(o->freq - freq) < 0.01f && fabsf(o->bw - bw) < 1; }

  // Listen before talk: another LoRa packet on this channel right now?
  int16_t scanChannel() {
    if (this == simCadBusy) return RADIOLIB_LORA_DETECTED;
    for (SX1262* o : all())
      if (o != this && o->txing && sameChannel(o) && rssiFrom(o) > sensitivity()) return RADIOLIB_LORA_DETECTED;
    return RADIOLIB_CHANNEL_FREE;
  }

  // SX1262 time on air, ms (explicit header, CRC on, coding rate 4/5)
  static double airtimeMs(size_t bytes, int sf, float bwKHz) {
    double ts = (1 << sf) / bwKHz;
    int de = ts > 16 ? 1 : 0;
    double n = 8.0 * bytes - 4 * sf + 28 + 16;
    int payload = 8 + (int)fmax(ceil(n / (4.0 * (sf - 2 * de))) * 5, 0);
    return (8 + 4.25 + payload) * ts;
  }

  // weaker with distance; a bit wider channel = a bit less sensitive
  float rssiFrom(const SX1262* o) const { float d = fmaxf(1.0f, fabsf(o->x - x)); return o->power - (36 + 32 * log10f(d)); }
  float sensitivity() const { return bw > 200 ? -117.0f : -120.0f; }

  int16_t startTransmit(const uint8_t* d, size_t len, uint8_t = 0) {
    txData.assign(d, d + len);
    txing = true;
    listening = false;
    txStart = simMillis;
    txDoneAt = simMillis + (unsigned long)ceil(airtimeMs(len, sf, bw));
    return 0;
  }

  // Called by the simulator every millisecond
  static void tickAir() {
    for (SX1262* s : all()) {
      if (!s->txing || simMillis < s->txDoneAt) continue;
      s->txing = false;
      simLoraSent++;
      bool lost = (rand() % 1000) < simLoraLoss * 1000 || s == simDeafTx;
      if (lost) simLoraLost++;
      for (SX1262* o : all()) {
        if (o == s || lost || !o->listening || o->txing || !o->sameChannel(s)) continue;
        float rssi = o->rssiFrom(s);
        float margin = rssi - o->sensitivity() + ((rand() % 400) - 200) / 100.0f;   // +-2 dB fading
        if (margin < 0) continue;
        // another packet on the air at the same time, heard here: lost (collision)
        bool hit = false;
        for (SX1262* t : all())
          if (t != s && t != o && t->sameChannel(s) && t->txStart < s->txDoneAt && t->txDoneAt > s->txStart && t->txDoneAt != 0 &&
              (t->txing || t->txDoneAt > s->txStart) && o->rssiFrom(t) > o->sensitivity() - 6 && fabsf(o->rssiFrom(t) - rssi) < 6) hit = true;
        if (hit) { simLoraCollided++; continue; }
        o->rxData = s->txData;
        o->lastRssi = rssi;
        o->lastSnr = margin - 5;
        if (o->cb) o->cb();
      }
      if (s->cb) s->cb();   // "transmit done"
    }
  }
};

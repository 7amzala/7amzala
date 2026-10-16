#pragma once
#include <cstdint>
#include <cstdlib>

// I2C bus for the simulator. With simADXL = true an ADXL355 answers at
// 0x53 and gives the simulated acceleration; otherwise nothing answers
// on the bus (the LIS3DH mock is used through its own library mock).
extern float simAX, simAY, simAZ;
void simRead();
inline bool simADXL = getenv("SIM_ADXL") != nullptr;

struct WireT
{
  uint8_t addr = 0, reg = 0, pos = 0, buf[16] = {0};
  int nWritten = 0;

  void begin() {}
  void setClock(long) {}
  void beginTransmission(uint8_t a) { addr = a; nWritten = 0; }
  void write(uint8_t v) { if(nWritten == 0) reg = v; nWritten++; }
  uint8_t endTransmission(bool = true) { return (simADXL && addr == 0x53) ? 0 : 2; }

  static void put20(uint8_t* b, float g)
  {
    long v = (long)(g * 256000.0f);
    if(v < 0) v += 0x100000;
    b[0] = (v >> 12) & 0xFF; b[1] = (v >> 4) & 0xFF; b[2] = (v << 4) & 0xF0;
  }

  uint8_t requestFrom(uint8_t a, uint8_t n)
  {
    if(!(simADXL && a == 0x53)) return 0;
    for(int i = 0; i < 16; i++) buf[i] = 0;
    if(reg == 0x00) { buf[0] = 0xAD; buf[1] = 0x1D; buf[2] = 0xED; }
    if(reg == 0x08) { simRead(); put20(buf, simAX); put20(buf + 3, simAY); put20(buf + 6, simAZ); }
    pos = 0;
    return n;
  }
  int read() { return buf[pos++]; }
};
extern WireT Wire;

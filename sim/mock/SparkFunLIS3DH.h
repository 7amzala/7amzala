#pragma once
#include <cstdint>
#define I2C_MODE 0
#define LIS3DH_CTRL_REG1 0x20
#define LIS3DH_CTRL_REG4 0x23
extern float simAX, simAY, simAZ;
void simRead();   // adds fresh sensor noise for each reading
struct SensorSettings { uint16_t accelSampleRate; uint8_t accelRange; };
class LIS3DH {
public:
  SensorSettings settings;
  LIS3DH(int,int){}
  int begin(){ return 0; }
  float readFloatAccelX(){ simRead(); return simAX; } float readFloatAccelY(){ return simAY; } float readFloatAccelZ(){ return simAZ; }
  uint8_t regs[64] = {0};
  int readRegister(uint8_t* o, uint8_t r){ *o=regs[r&63]; return 0; } int writeRegister(uint8_t r, uint8_t v){ regs[r&63]=v; return 0; }
};

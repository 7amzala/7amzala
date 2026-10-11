#pragma once
#include <cstdint>
#define I2C_MODE 0
#define LIS3DH_CTRL_REG4 0x23
extern float simAX, simAY, simAZ;
struct SensorSettings { uint16_t accelSampleRate; uint8_t accelRange; };
class LIS3DH {
public:
  SensorSettings settings;
  LIS3DH(int,int){}
  int begin(){ return 0; }
  float readFloatAccelX(){ return simAX; } float readFloatAccelY(){ return simAY; } float readFloatAccelZ(){ return simAZ; }
  int readRegister(uint8_t* o, uint8_t){ *o=0; return 0; } int writeRegister(uint8_t, uint8_t){ return 0; }
};

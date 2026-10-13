#pragma once
#include <cstdint>
#define MSBFIRST 1
#define SPI_MODE0 0
#define HSPI 1
struct SPISettings { SPISettings(uint32_t, uint8_t, uint8_t) {} };
struct SPIClass { SPIClass(int = 0) {} void begin(int, int, int, int = -1) {} };
extern SPIClass SPI;

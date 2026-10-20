#ifndef WATCH_IMU_H
#define WATCH_IMU_H

// Raise to wake: the board's QMI8658 motion sensor.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool imu_begin(void);         // after Touch_Init() (same I2C bus); false = no sensor
void imu_poll(void);          // from loop(): reads 25 times a second
bool imu_take_raise(void);    // true once after the wrist was raised to look

#ifdef __cplusplus
}
#endif

#endif

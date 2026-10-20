#ifndef WATCH_IMU_H
#define WATCH_IMU_H

// Looking at the watch: the board's QMI8658 motion sensor (accelerometer
// + gyroscope). The screen is on while you look, off when you look away.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool imu_begin(void);         // after Touch_Init() (same I2C bus); false = no sensor
void imu_poll(void);          // from loop(): reads 25 times a second
bool imu_present(void);       // false: no sensor (the screen then uses a timer)
bool imu_looking(void);       // you are looking at the watch now
bool imu_still(void);         // lying still (table): never "looking"

// TEACH MY LOOK: hold the watch as you look at it for 3 s
void imu_teach_start(void);
int imu_teach_state(void);    // 0 = idle, 1 = learning (hold still), 2 = done (learned), 3 = failed (moved)
float imu_teach_progress(void);   // 0..1 while learning

#ifdef __cplusplus
}
#endif

#endif

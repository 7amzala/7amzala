// =====================================================
// LAARIF STRIKESENSE WATCH (Bluetooth)
//
// Waveshare ESP32-S3-Touch-AMOLED-1.43. Talks to the rod over
// Bluetooth (no LoRa module needed): live graph, the rod's state and
// the buttons to answer. The phone app can be connected at the same time.
//
// Arduino IDE: Board "ESP32S3 Dev Module", Flash Size 16MB,
// Partition Scheme "Huge APP (3MB No OTA/1MB SPIFFS)", PSRAM "OPI PSRAM",
// USB CDC On Boot "Enabled" (for the [BLE] messages).
// Library: lvgl 8.3.x (as for the Waveshare demo).
// Optional: Pimoroni Haptic Bzzz for wrist vibration, on the header:
// SDA -> SDA, SCL -> SCL, 2-5V -> 3V3, GND -> GND (see buzz.c).
// =====================================================

#include "lcd_bsp.h"
#include "FT3168.h"
#include "ble_link.h"
#include "buzz.h"
#include "watch_bat.h"
#include "watch_imu.h"

void setup()
{
  Serial.begin(115200);

  uint32_t t = millis();          // give the Serial Monitor up to 3 s to connect
  while(!Serial && millis() - t < 3000)
    delay(10);

  Touch_Init();
  lcd_lvgl_Init();          // screen + the StrikeSense screens (watch_ui.c)
  buzz_begin();             // wrist vibration, if the Haptic Bzzz is wired (SDA/SCL)
  watch_bat_begin();        // the watch's own battery (BATTERY page)
  imu_begin();              // raise to wake (screen goes dark after 30 s)

  ble_link_begin();         // Bluetooth to the rod
}

void loop()
{
  ble_link_poll();          // find/connect the rod, live data, buttons
  watch_bat_poll();
  imu_poll();
  delay(2);
}

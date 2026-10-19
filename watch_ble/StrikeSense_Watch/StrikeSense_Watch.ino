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
// =====================================================

#include "lcd_bsp.h"
#include "FT3168.h"
#include "ble_link.h"

void setup()
{
  Serial.begin(115200);
  delay(500);

  Touch_Init();
  lcd_lvgl_Init();          // screen + the StrikeSense screens (watch_ui.c)

  ble_link_begin();         // Bluetooth to the rod
}

void loop()
{
  ble_link_poll();          // find/connect the rod, live data, buttons
  delay(2);
}

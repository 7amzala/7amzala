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
#include "watch_clock.h"
#include "strike_sd.h"
#include <esp_task_wdt.h>

// Watchdog: if the screen task or the Bluetooth loop is ever stuck for
// 30 s, the watch restarts by itself instead of staying frozen. (30 s:
// finding and connecting to the rod may take a while on purpose.)
static const uint32_t WATCHDOG_MS = 30000;

static void watchdogStart()
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = { WATCHDOG_MS, 0, true };

  if(esp_task_wdt_reconfigure(&cfg) != ESP_OK)
    esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WATCHDOG_MS / 1000, true);
#endif
}

static const char *resetReason()
{
  switch(esp_reset_reason())
  {
    case ESP_RST_POWERON:  return "power";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "low battery";
    default:               return "other";
  }
}

void setup()
{
  Serial.begin(115200);

  uint32_t t = millis();          // give the Serial Monitor up to 3 s to connect
  while(!Serial && millis() - t < 3000)
    delay(10);

  Serial.printf("[WATCH] started after: %s\n", resetReason());
  watchdogStart();          // before the screen task starts (it checks in too)

  Touch_Init();
  clock_begin();            // time and date (clock chip on the touch bus)
  strike_sd_begin();        // micro-SD card: strike history (before the screens use it)
  lcd_lvgl_Init();          // screen + the StrikeSense screens (watch_ui.c)
  buzz_begin();             // wrist vibration, if the Haptic Bzzz is wired (SDA/SCL)
  watch_bat_begin();        // the watch's own battery (BATTERY page)
  imu_begin();              // raise to wake (screen goes dark after 30 s)

  ble_link_begin();         // Bluetooth to the rod

  esp_task_wdt_add(NULL);   // from here on the loop below checks in too
}

void loop()
{
  esp_task_wdt_reset();     // "still alive"

  ble_link_poll();          // find/connect the rod, live data, buttons
  watch_bat_poll();
  imu_poll();
  delay(2);
}

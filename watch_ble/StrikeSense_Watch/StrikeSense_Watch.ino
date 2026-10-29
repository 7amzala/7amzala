// =====================================================
// LAARIF STRIKESENSE WATCH (Bluetooth + LoRa)
//
// Waveshare ESP32-S3-Touch-AMOLED-1.43. Talks to the rod over
// Bluetooth: live graph, the rod's state and the buttons to answer.
// The phone app can be connected at the same time.
// With the SX1262 LoRa Node wired (see WATCH.md) and the LoRa rod sketch
// (StrikeSense.ino), the watch switches to LoRa by itself when the rod
// is too far for Bluetooth, and back when it is near again.
// No LoRa module? It still works over Bluetooth (or set WATCH_LORA 0
// in lora_link.h).
//
// Arduino IDE: Board "ESP32S3 Dev Module", Flash Size 16MB,
// Partition Scheme "Huge APP (3MB No OTA/1MB SPIFFS)", PSRAM "OPI PSRAM",
// USB CDC On Boot "Enabled" (for the [BLE] messages).
// Libraries: lvgl 8.3.x (as for the Waveshare demo), RadioLib.
// Optional: Pimoroni Haptic Bzzz for wrist vibration, on the header:
// SDA -> SDA, SCL -> SCL, 2-5V -> 3V3, GND -> GND (see buzz.c).
// =====================================================

#include "lcd_bsp.h"
#include "FT3168.h"
#include "ble_link.h"
#include "lora_link.h"
#include "buzz.h"
#include "watch_bat.h"
#include "watch_imu.h"
#include "watch_clock.h"
#include "strike_sd.h"
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "esp_app_desc.h"

// Watchdog: if the screen task, the Bluetooth loop or the LoRa task is ever stuck for
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

// A new sketch uploaded (not a restart): a fresh watch. It forgets its
// rod (pair again over Bluetooth with the rod's PIN), the LoRa key, LoRa
// only and the taught look. Kept: the clock and the strike history on the
// SD card. The program's fingerprint changes with every new upload.
static bool freshAfterUpload()
{
  char sha[17] = { 0 };
  esp_app_get_elf_sha256(sha, sizeof(sha));

  Preferences p;
  p.begin("fresh", false);
  bool fresh = p.getString("build", "") != String(sha);

  if(fresh)
    p.putString("build", sha);

  p.end();

  if(!fresh)
    return false;

  // LoRa packet numbers go on from before (the rod refuses old numbers)
  p.begin("pair", false);
  uint32_t lboot = p.getULong("lboot", 0);
  p.clear();

  if(lboot)
    p.putULong("lboot", lboot);

  p.end();

  p.begin("look", false);
  p.clear();
  p.end();

  Serial.println("[WATCH] NEW SKETCH UPLOADED: a fresh watch - pair it with the rod again (PIN)");
  return true;
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

// Paired (pair_ui.c) or the rod forgot this watch: start again in the right screens
// a line on the Serial Monitor from the screens (C code)
extern "C" void watch_say(const char *t)
{
  Serial.println(t);
}

extern "C" void pair_restart(void)
{
  delay(200);
  ESP.restart();
}

void setup()
{
  Serial.begin(115200);

  uint32_t t = millis();          // give the Serial Monitor up to 3 s to connect
  while(!Serial && millis() - t < 3000)
    delay(10);

  Serial.printf("[WATCH] started after: %s\n", resetReason());
  watchdogStart();          // before the screen task starts (it checks in too)
  bool fresh = freshAfterUpload();

  Touch_Init();
  clock_begin();            // time and date (clock chip on the touch bus)
  strike_sd_begin();        // micro-SD card: strike history (before the screens use it)
  lcd_lvgl_Init();          // screen + the StrikeSense screens (watch_ui.c)
  buzz_begin();             // wrist vibration, if the Haptic Bzzz is wired (SDA/SCL)
  watch_bat_begin();        // the watch's own battery (BATTERY page)
  imu_begin();              // raise to wake (screen goes dark after 30 s)

  ble_link_begin();         // Bluetooth to the rod

  if(fresh)
    ble_link_forget();      // new sketch: the rod's pairing too (Bluetooth bonds)
  lora_link_start_task();   // LoRa to the rod (its own task: never misses a packet)

  esp_task_wdt_add(NULL);   // from here on the loop below checks in too
}

void loop()
{
  esp_task_wdt_reset();     // "still alive"

  // memory, every 30 s (Bluetooth needs fast internal memory: if it runs
  // low, Bluetooth data is lost and the watch can crash)
  static uint32_t memSaidAt = 0;

  if(millis() - memSaidAt >= 30000)
  {
    memSaidAt = millis();
    Serial.printf("[WATCH] memory: internal %u KB free (biggest block %u KB), PSRAM %u KB free\n",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  }

  ble_link_poll();          // find/connect the rod, live data, buttons
  watch_bat_poll();
  imu_poll();
  delay(2);
}

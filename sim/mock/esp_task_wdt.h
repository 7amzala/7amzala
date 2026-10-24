#pragma once
// Host stand-in for the ESP32 task watchdog: counts the "still alive"
// calls and remembers the longest gap between them (tests check it).
#include <cstdint>
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 3
#endif
typedef int esp_err_t;
typedef struct { uint32_t timeout_ms; uint32_t idle_core_mask; bool trigger_panic; } esp_task_wdt_config_t;
typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT,
               ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO } esp_reset_reason_t;
struct WdtMock { bool on = false; uint32_t timeout = 0; unsigned long last = 0, longest = 0; long resets = 0; };
inline WdtMock& wdtMock() { static WdtMock w; return w; }
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
inline esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t* c) { wdtMock().timeout = c->timeout_ms; return ESP_OK; }
inline esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t* c) { wdtMock().timeout = c->timeout_ms; return ESP_OK; }
inline esp_err_t esp_task_wdt_add(void*) { extern unsigned long simMillis; wdtMock().on = true; wdtMock().last = simMillis; return ESP_OK; }
inline esp_err_t esp_task_wdt_reset()
{
  extern unsigned long simMillis;
  WdtMock& w = wdtMock();
  if(w.on) { if(simMillis - w.last > w.longest) w.longest = simMillis - w.last; w.last = simMillis; w.resets++; }
  return ESP_OK;
}

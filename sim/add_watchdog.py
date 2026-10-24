#!/usr/bin/env python3
"""Rod: watchdog. If the program ever gets stuck for 10 s, the chip
restarts by itself (instead of a frozen rod all night). After a restart
the reason is printed and sent to the phone ("rst" in /data); the app
shows a note when the rod restarted by itself.

    python3 add_watchdog.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"{path}: expected {count} x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


rep('''#include <Preferences.h>
''', '''#include <Preferences.h>
#include <esp_task_wdt.h>

// (defined further down, before the battery code)
void startWatchdog();
const char* resetReason();
''')



rep('''// ---------- Battery ----------

// Resting LiPo voltage -> percent''', '''// ---------- Watchdog ----------

const uint32_t WATCHDOG_MS = 10000;      // stuck this long = restart

// loop() must check in at least every WATCHDOG_MS, or the chip restarts
void startWatchdog()
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = { WATCHDOG_MS, 0, true };

  if(esp_task_wdt_reconfigure(&cfg) != ESP_OK)
    esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WATCHDOG_MS / 1000, true);
#endif

  esp_task_wdt_add(NULL);               // this task: setup() + loop()
}

// why the rod started this time
const char* resetReason()
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
    case ESP_RST_DEEPSLEEP: return "sleep";
    default:               return "other";
  }
}

// ---------- Battery ----------

// Resting LiPo voltage -> percent''')

rep('''  addField(json, "strikeready", lastStrikeReady);''', '''  addField(json, "strikeready", lastStrikeReady);
  json += ",\\"rst\\":\\"";
  json += resetReason();
  json += "\\"";''')

rep('''  startEnvironmentLearning();

  lastSampleTime = millis();
}''', '''  startEnvironmentLearning();

  lastSampleTime = millis();

  Serial.printf("Started after: %s\\n", resetReason());
  startWatchdog();                      // from here on: stuck 10 s = restart
}''')

rep('''void loop()
{
  server.handleClient();''', '''void loop()
{
  esp_task_wdt_reset();                 // "still alive" for the watchdog

  server.handleClient();''')

# phone app: a note when the rod restarted by itself
rep('''    // ---------- Replay availability ----------''', '''    // ---------- The rod restarted by itself (watchdog, crash, low battery)? ----------

    if(d.rst && d.rst !== "power" && d.rst !== "software" && !rstShown)
    {
      rstShown = true;
      const t = el("savedToast");
      t.innerText = "\\u26A0 The rod restarted by itself (" + d.rst + ") \\u2013 it is watching again";
      t.style.color = "#ffba52";
      t.style.borderColor = "#ffba52";
      t.classList.add("show");
      setTimeout(() => { t.classList.remove("show"); t.innerText = "\\u2713 Saved"; t.style.color = ""; t.style.borderColor = ""; }, 8000);
    }

    // ---------- Replay availability ----------''')

rep('''let replayTryAt = 0;''', '''let replayTryAt = 0;
let rstShown = false;''')

open(path, 'w').write(s)
print("ok", path)

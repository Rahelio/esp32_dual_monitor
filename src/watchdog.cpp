#include "watchdog.h"

#include <esp_task_wdt.h>

namespace {
// Comfortably above the longest single blocking call in this firmware
// (syncTime()'s 15s NTP wait) so a slow-but-successful boot never trips it.
const uint32_t WDT_TIMEOUT_S = 25;
}  // namespace

void setupWatchdog() {
  // This is the esp_task_wdt API for recent (IDF5-based) arduino-esp32 cores,
  // which is what a fresh `pio pkg install` pulls as of this writing. If
  // your installed core predates that, this call won't compile -- swap it
  // for the older 2-argument form instead:
  //   esp_task_wdt_init(WDT_TIMEOUT_S, true);
  //   esp_task_wdt_add(NULL);
  esp_task_wdt_config_t wdtConfig = {
      .timeout_ms = WDT_TIMEOUT_S * 1000,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  esp_task_wdt_init(&wdtConfig);
  esp_task_wdt_add(NULL);  // watch this task (Arduino's loop task)
}

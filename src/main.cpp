// ESP32-S3-Zero physical dashboard firmware.
//
// Layout: the two screens act as one master-detail pair, cycling through
// "host", then each container in turn. Left screen shows which item is
// currently up (big name + page indicator), right screen shows that
// item's stats. Two independent timers: FETCH_INTERVAL_MS pulls fresh
// data from the aggregator, PAGE_INTERVAL_MS advances which item is
// currently shown -- decoupled so the display keeps cycling smoothly
// even between fetches.
//
// Screensaver: after IDLE_TIMEOUT_MS with no BOOT button press, the display
// switches from the data cycle to a screensaver -- a live clock (NTP-synced,
// DST-aware) with a bouncing DVD-logo icon on the left, and on the right a
// repeating cycle of weather (current conditions / today's range, alternating)
// interleaved with a few fun full-screen animations (starfield, equalizer,
// matrix rain), each shown for RIGHT_PAGE_DURATION_MS. Every so often the two
// panels are treated as one wide canvas for a short Pong interlude, ball and
// auto-tracking paddles crossing the seam between them. A container that's
// gone down recently pre-empts all of that -- both panels blink a warning
// until it's cleared. Pressing BOOT wakes back to data mode (a quick tap
// there also skips to the next page; holding it forces an immediate
// dashboard refresh). Both panels dim automatically overnight.
//
// The Open-Meteo call is the only HTTPS in this firmware; it skips
// certificate verification (WiFiClientSecure::setInsecure()) rather than
// pinning a cert -- deliberate simplification since it's read-only public
// data with no credentials involved.
//
// Implementation is split across src/*.cpp by concern -- see each file's
// own header comment for what it owns:
//   watchdog       - task watchdog, reboots on a hung main loop
//   display_hw     - the two SSD1306 panels + I2C buses
//   draw_helpers   - small reusable drawing/formatting primitives
//   wifi_manager   - WiFi connect/reconnect
//   time_sync      - NTP sync + night/manual dimming
//   weather        - Open-Meteo fetch
//   dashboard      - aggregator fetch + data-mode item/paging state
//   dashboard_view - data-mode rendering (renderCurrentItem)
//   mode           - DisplayMode + idle-activity state shared across modules
//   button         - BOOT button debounce/tap/hold handling
//   animations     - full-screen screensaver animations
//   screensaver    - screensaver orchestration (clock, weather cycle, alert, Pong)

#include <Arduino.h>
#include <Wire.h>
#include <esp_task_wdt.h>

#include "button.h"
#include "config.h"
#include "dashboard.h"
#include "dashboard_view.h"
#include "display_hw.h"
#include "mode.h"
#include "screensaver.h"
#include "time_sync.h"
#include "watchdog.h"
#include "weather.h"
#include "wifi_manager.h"

namespace {
unsigned long lastWeatherFetch = 0;
unsigned long lastNightCheck = 0;
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);  // give native-USB serial a moment to enumerate
  Serial.println("\nDashboard firmware starting");

  setupWatchdog();  // armed before anything that could conceivably hang

  Wire.begin(DISP1_SDA, DISP1_SCL);
  I2CBus2.begin(DISP2_SDA, DISP2_SCL);

  bool ok1 = bringUpDisplay(display1, "Display 1");
  bool ok2 = bringUpDisplay(display2, "Display 2");
  if (!(ok1 && ok2)) {
    Serial.println("At least one display did not respond -- check SDA/SCL "
                    "wiring, power, and that both modules are on 0x3C.");
  }
  delay(1000);
  showBootSplash();

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  connectWiFi();
  syncTime();

  if (fetchDashboard()) {
    dashboardValid = true;
    logDashboard();
  }
  lastFetch = millis();

  if (fetchWeather(lastWeather)) {
    Serial.printf("Weather: %.1fC code=%d isDay=%d\n", lastWeather.tempC,
                  lastWeather.weatherCode, lastWeather.isDay);
  }
  lastWeatherFetch = millis();

  updateNightDimming();  // set the correct initial contrast rather than
                          // waiting up to NIGHT_CHECK_INTERVAL_MS after boot
  lastNightCheck = millis();

  lastActivityMs = millis();
  renderCurrentItem();
}

void loop() {
  esp_task_wdt_reset();
  ensureWiFiConnected();
  handleButton();

  if (millis() - lastNightCheck >= NIGHT_CHECK_INTERVAL_MS) {
    lastNightCheck = millis();
    updateNightDimming();
  }

  // Data (and weather) keep fetching in the background regardless of mode,
  // so both are fresh the moment the screensaver is dismissed or entered.
  if (millis() - lastFetch >= FETCH_INTERVAL_MS) {
    lastFetch = millis();
    if (fetchDashboard()) {
      dashboardValid = true;
      logDashboard();
      if (currentMode == MODE_DATA) renderCurrentItem();
    }
  }

  if (millis() - lastWeatherFetch >= WEATHER_FETCH_INTERVAL_MS) {
    lastWeatherFetch = millis();
    WeatherData w;
    if (fetchWeather(w)) {
      lastWeather = w;
      Serial.printf("Weather: %.1fC code=%d isDay=%d\n", w.tempC,
                    w.weatherCode, w.isDay);
    }
  }

  if (currentMode == MODE_DATA) {
    if (millis() - lastActivityMs >= IDLE_TIMEOUT_MS) {
      enterScreensaver();  // also forces an immediate screensaver redraw
    } else if (dashboardValid && itemCount > 0 &&
               millis() - lastPageSwitch >= PAGE_INTERVAL_MS) {
      lastPageSwitch = millis();
      currentItem = (currentItem + 1) % itemCount;
      renderCurrentItem();
    }
  } else {  // MODE_SCREENSAVER
    if (screensaverTickDue()) {
      renderScreensaver();
    }
  }
}

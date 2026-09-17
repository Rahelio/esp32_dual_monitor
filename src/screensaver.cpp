#include "screensaver.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

#include "animations.h"
#include "config.h"
#include "dashboard.h"
#include "display_hw.h"
#include "draw_helpers.h"
#include "mode.h"
#include "weather.h"

// ---- DVD-logo bounce, clock screen's yellow strip (y0-15) ----
namespace {
// Float position + px/sec speed (rather than px/tick) so the pace stays the
// same no matter what SCREENSAVER_TICK_MS is -- this is the animation the
// user specifically said to keep as-is.
const int DVD_ICON_SIZE = 8;
const float DVD_SPEED = 3.0f;  // px/sec, matches the original 3px-per-1000ms pace
const int DVD_STRIP_HEIGHT = 16;
float dvdX = 20, dvdY = 4;
float dvdDirX = 1, dvdDirY = 1;
int dvdShape = 0;  // cycles to a different shape on each wall hit

void drawDvdShape(Adafruit_SSD1306 &d, int x, int y, int shape) {
  switch (shape % 3) {
    case 0:
      d.fillRect(x, y, DVD_ICON_SIZE, DVD_ICON_SIZE, SSD1306_WHITE);
      break;
    case 1:
      d.fillCircle(x + DVD_ICON_SIZE / 2, y + DVD_ICON_SIZE / 2, DVD_ICON_SIZE / 2,
                   SSD1306_WHITE);
      break;
    case 2:
      d.fillTriangle(x, y + DVD_ICON_SIZE, x + DVD_ICON_SIZE / 2, y, x + DVD_ICON_SIZE,
                      y + DVD_ICON_SIZE, SSD1306_WHITE);
      break;
  }
}

// Classic bouncing-logo screensaver bit: moves at a constant px/sec speed
// (via dt, not a fixed step per tick), reverses direction off each wall, and
// swaps to a different tiny shape on every bounce (no color to change on a
// monochrome panel, so the shape is the "new logo" moment instead). Confined
// to the yellow strip so it never competes with the clock text below it.
void updateAndDrawDvdBounce(Adafruit_SSD1306 &d, float dt) {
  dvdX += dvdDirX * DVD_SPEED * dt;
  dvdY += dvdDirY * DVD_SPEED * dt;

  bool bounced = false;
  if (dvdX <= 0) {
    dvdX = 0;
    dvdDirX = 1;
    bounced = true;
  } else if (dvdX >= SCREEN_WIDTH - DVD_ICON_SIZE) {
    dvdX = SCREEN_WIDTH - DVD_ICON_SIZE;
    dvdDirX = -1;
    bounced = true;
  }

  if (dvdY <= 0) {
    dvdY = 0;
    dvdDirY = 1;
    bounced = true;
  } else if (dvdY >= DVD_STRIP_HEIGHT - DVD_ICON_SIZE) {
    dvdY = DVD_STRIP_HEIGHT - DVD_ICON_SIZE;
    dvdDirY = -1;
    bounced = true;
  }

  if (bounced) dvdShape++;

  drawDvdShape(d, (int)dvdX, (int)dvdY, dvdShape);
}
}  // namespace

// ---- Right-display page cycle: weather interleaved with animations ----
namespace {
enum RightPage {
  RP_WEATHER,
  RP_ANIM_STARFIELD,
  RP_ANIM_PULSE,
  RP_ANIM_LOADBARS,
  RP_ANIM_FLEET,
  RP_ANIM_RAIN,
};
const RightPage RIGHT_PAGE_SEQUENCE[] = {
  RP_WEATHER, RP_ANIM_STARFIELD, RP_WEATHER, RP_ANIM_PULSE,
  RP_WEATHER, RP_ANIM_LOADBARS,  RP_WEATHER, RP_ANIM_FLEET,
  RP_WEATHER, RP_ANIM_RAIN,
};
const int RIGHT_PAGE_COUNT = 10;
const unsigned long RIGHT_PAGE_DURATION_MS = 5000;
int rightPageIndex = 0;
unsigned long lastRightPageSwitch = 0;
bool weatherShowForecast = false;  // toggled each time a weather slot starts

// Two-tone panel: icon confined to the yellow strip (y0-15), everything
// else -- temp, label, any animated accent -- in the blue.
void renderWeatherPage(Adafruit_SSD1306 &d, bool showForecast) {
  if (!lastWeather.valid) {
    d.setTextSize(1);
    d.setCursor(0, 20);
    d.println("Weather data");
    d.println("unavailable");
    return;
  }

  int code = lastWeather.weatherCode;
  const int iconX = 4, iconY = 1;

  // Icon stays up regardless of which sub-page is showing below it.
  if (code == 0 || code == 1) {
    if (lastWeather.isDay) drawSunIcon(d, iconX, iconY);
    else drawMoonIcon(d, iconX, iconY);
  } else {
    drawCloudIcon(d, iconX, iconY);
  }

  if (showForecast) {
    d.setTextSize(1);
    d.setCursor(0, 20);
    d.println("Today's range:");
    d.setTextSize(2);
    d.setCursor(0, 34);
    d.printf("%.0f/%.0f", lastWeather.tempMaxC, lastWeather.tempMinC);
    d.setTextSize(1);
    d.setCursor(0, 54);
    if (lastWeather.precipProbMax >= 0) {
      d.printf("Rain: %d%%", lastWeather.precipProbMax);
    } else {
      d.print("Rain: n/a");
    }
  } else {
    d.setTextSize(2);
    d.setCursor(0, 18);
    d.printf("%.0fC", lastWeather.tempC);

    d.setTextSize(1);
    d.setCursor(0, 38);
    d.println(weatherLabel(code));

    int accentFrame = millis() / 1000;
    const int accentX = 4, accentY = 50;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
      drawRainAccent(d, accentX, accentY, accentFrame);
    } else if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
      drawSnowAccent(d, accentX, accentY, accentFrame);
    } else if (code >= 95) {
      drawStormAccent(d, accentX, accentY, accentFrame);
    } else if (code == 45 || code == 48) {
      drawFogAccent(d, accentX, accentY, accentFrame);
    }
  }
}

// Advances rightPageIndex on its own timer (RIGHT_PAGE_DURATION_MS), and
// flips which weather sub-page shows next each time a weather slot starts --
// independent of the screensaver's own (much faster) redraw tick.
void updateRightPage() {
  if (lastRightPageSwitch == 0) lastRightPageSwitch = millis();
  if (millis() - lastRightPageSwitch >= RIGHT_PAGE_DURATION_MS) {
    lastRightPageSwitch = millis();
    rightPageIndex = (rightPageIndex + 1) % RIGHT_PAGE_COUNT;
    if (RIGHT_PAGE_SEQUENCE[rightPageIndex] == RP_WEATHER) {
      weatherShowForecast = !weatherShowForecast;
    }
  }
}
}  // namespace

// ---- Pong interlude: every PONG_INTERVAL_MS, takes over BOTH panels for
// PONG_DURATION_MS (see animations.cpp for the actual rendering). ----
namespace {
const unsigned long PONG_INTERVAL_MS = 40000;
const unsigned long PONG_DURATION_MS = 6000;
bool pongActive = false;
unsigned long pongStartMs = 0;
unsigned long lastPongTrigger = 0;
}  // namespace

// ---- Downtime alert ----
namespace {
// Scans the last-fetched containers for one that's down_recently. Only
// surfaces the first match -- if more than one container is down at once
// this won't cycle between them, just a deliberate simplification rather
// than a separate rotation timer for what should be a rare state.
bool findDowntimeAlert(String &name, long &downSeconds) {
  if (!dashboardValid) return false;
  JsonArray containers = dashboardDoc["containers"];
  if (containers.isNull()) return false;
  for (JsonObject c : containers) {
    if (c["down_recently"] | false) {
      name = String((const char *)(c["name"] | "?"));
      downSeconds = c["down_duration_seconds"] | 0L;
      return true;
    }
  }
  return false;
}

// Both panels blink together (on screen, then blank) so it reads as an
// alarm rather than just another page in the rotation. Pre-empts everything
// else in the screensaver -- clock, weather cycle, animations, Pong -- for
// as long as the alert holds, so nothing keeps animating unnoticed behind it.
void renderScreensaverAlert(const String &name, long downSeconds) {
  bool blinkOn = (millis() % 900) < 600;  // ~600ms on, ~300ms off
  if (!blinkOn) {
    display1.display();
    display2.display();
    return;
  }

  const int iconSize = 14;
  drawWarningIcon(display1, (SCREEN_WIDTH - iconSize) / 2, 1, iconSize);
  drawWarningIcon(display2, (SCREEN_WIDTH - iconSize) / 2, 1, iconSize);

  display1.setTextSize(2);
  display1.setCursor(0, 20);
  display1.println("DOWN");
  display1.setTextSize(1);
  display1.setCursor(0, 46);
  display1.println(truncated(name.c_str(), 16));

  display2.setTextSize(1);
  display2.setCursor(0, 20);
  display2.println("last up:");
  display2.setTextSize(2);
  display2.setCursor(0, 32);
  display2.println(formatDuration(downSeconds));
  display2.setTextSize(1);
  display2.setCursor(0, 54);
  display2.println("ago");

  display1.display();
  display2.display();
}
}  // namespace

// Real elapsed time between screensaver redraws, used to drive all motion
// by dt (px/sec) instead of a fixed px-per-tick step -- so animation speed
// stays correct regardless of the tick rate, and a long gap (e.g. first
// frame after waking from an idle MODE_DATA stretch) doesn't jump.
namespace {
unsigned long lastScreensaverUpdateMs = 0;
}  // namespace

void enterScreensaver() {
  Serial.println("Idle timeout -- entering screensaver");
  currentMode = MODE_SCREENSAVER;
  lastScreensaverUpdateMs = 0;  // dt starts fresh, no bogus first-frame jump
  rightPageIndex = 0;           // always wake into weather, not mid-animation
  lastRightPageSwitch = 0;
  pongActive = false;  // Pong interlude timer restarts fresh too
  lastPongTrigger = millis();
}

void renderScreensaver() {
  unsigned long now = millis();
  float dt = (lastScreensaverUpdateMs == 0) ? 0.0f : (now - lastScreensaverUpdateMs) / 1000.0f;
  lastScreensaverUpdateMs = now;
  if (dt > 0.5f) dt = 0.5f;  // clamp a long gap (e.g. just woke from MODE_DATA)

  display1.clearDisplay();
  display2.clearDisplay();
  display1.setTextColor(SSD1306_WHITE);
  display2.setTextColor(SSD1306_WHITE);

  // ---- downtime alert pre-empts everything else ----
  String alertName;
  long alertDownSeconds = 0;
  if (findDowntimeAlert(alertName, alertDownSeconds)) {
    renderScreensaverAlert(alertName, alertDownSeconds);
    return;
  }

  // ---- Pong interlude also takes over both panels, but yields to an alert ----
  if (!pongActive && millis() - lastPongTrigger >= PONG_INTERVAL_MS) {
    pongActive = true;
    pongStartMs = millis();
  }
  if (pongActive && millis() - pongStartMs >= PONG_DURATION_MS) {
    pongActive = false;
    lastPongTrigger = millis();
  }
  if (pongActive) {
    renderPong(dt);
    display1.display();
    display2.display();
    return;
  }

  // ---- left: bouncing icon (yellow strip) + clock (blue) ----
  updateAndDrawDvdBounce(display1, dt);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    char timeStr[10];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    char dateStr[16];
    strftime(dateStr, sizeof(dateStr), "%a %d %b", &timeinfo);

    display1.setTextSize(2);
    display1.setCursor(16, 20);
    display1.println(timeStr);
    display1.setTextSize(1);
    display1.setCursor(10, 48);
    display1.println(dateStr);
  } else {
    display1.setTextSize(1);
    display1.setCursor(0, 0);
    display1.println("Time not");
    display1.println("synced yet");
  }

  // ---- right: weather, interleaved with animations on a timer ----
  updateRightPage();
  switch (RIGHT_PAGE_SEQUENCE[rightPageIndex]) {
    case RP_WEATHER:
      renderWeatherPage(display2, weatherShowForecast);
      break;
    case RP_ANIM_STARFIELD:
      renderStarfield(display2, dt);
      break;
    case RP_ANIM_PULSE:
      renderSystemPulse(display2, dt);
      break;
    case RP_ANIM_LOADBARS:
      renderLoadBars(display2, dt);
      break;
    case RP_ANIM_FLEET:
      renderFleetGrid(display2, dt);
      break;
    case RP_ANIM_RAIN:
      renderMatrixRain(display2, dt);
      break;
  }

  display1.display();
  display2.display();
}

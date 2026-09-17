#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>

String truncated(const char *s, size_t maxLen);
String formatDuration(long totalSeconds);
String formatRate(float bytesPerSec);

void drawWarningIcon(Adafruit_SSD1306 &d, int x, int y, int size);

// ---- Weather icons -- compact, sized to fit the ~14px-tall yellow strip ----
void drawSunIcon(Adafruit_SSD1306 &d, int x, int y);
void drawMoonIcon(Adafruit_SSD1306 &d, int x, int y);
void drawCloudIcon(Adafruit_SSD1306 &d, int x, int y);

// Small animated accents, drawn in the blue zone below the main icon.
// `frame` drives slow, deliberate motion (~1 step/sec) independent of the
// screensaver's own redraw rate -- callers pass millis()/1000 so these stay
// paced the same regardless of how often renderWeatherPage() is called.
void drawRainAccent(Adafruit_SSD1306 &d, int x, int y, int frame);
void drawSnowAccent(Adafruit_SSD1306 &d, int x, int y, int frame);
void drawStormAccent(Adafruit_SSD1306 &d, int x, int y, int frame);
void drawFogAccent(Adafruit_SSD1306 &d, int x, int y, int frame);

const char *weatherLabel(int code);
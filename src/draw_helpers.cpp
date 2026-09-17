#include "draw_helpers.h"

String truncated(const char *s, size_t maxLen) {
  String out(s);
  if (out.length() > maxLen) out = out.substring(0, maxLen);
  return out;
}

String formatDuration(long totalSeconds) {
  if (totalSeconds < 60) return String(totalSeconds) + "s";
  long minutes = totalSeconds / 60;
  if (minutes < 60) return String(minutes) + "m";
  long hours = minutes / 60;
  minutes = minutes % 60;
  return String(hours) + "h " + String(minutes) + "m";
}

String formatRate(float bytesPerSec) {
  if (bytesPerSec >= 1024.0f * 1024.0f) {
    return String(bytesPerSec / (1024.0f * 1024.0f), 1) + "M";
  } else if (bytesPerSec >= 1024.0f) {
    return String(bytesPerSec / 1024.0f, 0) + "K";
  }
  return String((int)bytesPerSec) + "B";
}

void drawWarningIcon(Adafruit_SSD1306 &d, int x, int y, int size) {
  d.drawTriangle(x, y + size, x + size / 2, y, x + size, y + size, SSD1306_WHITE);
  d.fillRect(x + size / 2 - 1, y + size / 3, 2, size / 3, SSD1306_WHITE);
  d.fillRect(x + size / 2 - 1, y + size - 6, 2, 2, SSD1306_WHITE);
}

void drawSunIcon(Adafruit_SSD1306 &d, int x, int y) {
  int cx = x + 7, cy = y + 7;
  d.fillCircle(cx, cy, 4, SSD1306_WHITE);
  d.drawLine(cx, y, cx, y + 1, SSD1306_WHITE);
  d.drawLine(cx, y + 12, cx, y + 13, SSD1306_WHITE);
  d.drawLine(x, cy, x + 1, cy, SSD1306_WHITE);
  d.drawLine(x + 13, cy, x + 14, cy, SSD1306_WHITE);
  d.drawPixel(x + 2, y + 2, SSD1306_WHITE);
  d.drawPixel(x + 12, y + 2, SSD1306_WHITE);
  d.drawPixel(x + 2, y + 12, SSD1306_WHITE);
  d.drawPixel(x + 12, y + 12, SSD1306_WHITE);
}

void drawMoonIcon(Adafruit_SSD1306 &d, int x, int y) {
  d.fillCircle(x + 7, y + 7, 6, SSD1306_WHITE);
  d.fillCircle(x + 10, y + 5, 6, SSD1306_BLACK);
}

void drawCloudIcon(Adafruit_SSD1306 &d, int x, int y) {
  d.fillCircle(x + 4, y + 9, 4, SSD1306_WHITE);
  d.fillCircle(x + 9, y + 6, 5, SSD1306_WHITE);
  d.fillCircle(x + 15, y + 9, 4, SSD1306_WHITE);
  d.fillRect(x, y + 9, 19, 5, SSD1306_WHITE);
}

void drawRainAccent(Adafruit_SSD1306 &d, int x, int y, int frame) {
  int offset = frame % 3;
  for (int i = 0; i < 3; i++) {
    int dx = x + i * 8;
    int dy = y + offset * 2;
    d.drawLine(dx, dy, dx - 2, dy + 4, SSD1306_WHITE);
  }
}

void drawSnowAccent(Adafruit_SSD1306 &d, int x, int y, int frame) {
  int offset = frame % 4;
  for (int i = 0; i < 3; i++) {
    d.fillCircle(x + i * 8, y + offset * 2, 1, SSD1306_WHITE);
  }
}

void drawStormAccent(Adafruit_SSD1306 &d, int x, int y, int frame) {
  if (frame % 2 != 0) return;  // blink the bolt
  d.drawLine(x + 4, y, x, y + 6, SSD1306_WHITE);
  d.drawLine(x, y + 6, x + 3, y + 6, SSD1306_WHITE);
  d.drawLine(x + 3, y + 6, x - 1, y + 12, SSD1306_WHITE);
}

void drawFogAccent(Adafruit_SSD1306 &d, int x, int y, int frame) {
  (void)frame;  // static accent, kept for signature symmetry with the others
  for (int i = 0; i < 3; i++) {
    d.drawFastHLine(x, y + i * 4, 24, SSD1306_WHITE);
  }
}

const char *weatherLabel(int code) {
  if (code == 0) return "Clear";
  if (code >= 1 && code <= 2) return "Mostly clear";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code >= 85 && code <= 86) return "Snow shwr";
  if (code >= 95) return "T-storm";
  return "Unknown";
}
#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Wire.h>

extern TwoWire I2CBus2;  // second hardware I2C controller
extern Adafruit_SSD1306 display1;
extern Adafruit_SSD1306 display2;

bool bringUpDisplay(Adafruit_SSD1306 &display, const char *label);
void showStatus(const String &line1, const String &line2);

// display1 shows live progress via showStatus() through the rest of boot
// (WiFi SSID, connected, syncing time, ...) -- display2 has nothing similar,
// so this gives it a static splash to hold instead, drawn once and left up
// until the first renderCurrentItem() call at the end of setup() overwrites
// it.
void showBootSplash();
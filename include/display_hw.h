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
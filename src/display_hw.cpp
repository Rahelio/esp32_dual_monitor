#include "display_hw.h"

#include "config.h"

TwoWire I2CBus2 = TwoWire(1);  // second hardware I2C controller

Adafruit_SSD1306 display1(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_SSD1306 display2(SCREEN_WIDTH, SCREEN_HEIGHT, &I2CBus2, OLED_RESET);

bool bringUpDisplay(Adafruit_SSD1306 &display, const char *label) {
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.printf("[%s] SSD1306 init FAILED (check wiring/address)\n", label);
    return false;
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(label);
  display.println("OK - I2C bus alive");
  display.display();
  Serial.printf("[%s] SSD1306 init OK\n", label);
  return true;
}

void showStatus(const String &line1, const String &line2) {
  display1.clearDisplay();
  display1.setCursor(0, 0);
  display1.setTextSize(1);
  display1.println(line1);
  display1.println(line2);
  display1.display();
}
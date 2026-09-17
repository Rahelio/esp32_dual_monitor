#include "dashboard_view.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#include "config.h"
#include "dashboard.h"
#include "display_hw.h"
#include "draw_helpers.h"

void renderCurrentItem() {
  display1.clearDisplay();
  display2.clearDisplay();
  display1.setTextColor(SSD1306_WHITE);
  display2.setTextColor(SSD1306_WHITE);

  if (!dashboardValid || itemCount == 0) {
    display1.setTextSize(1);
    display1.setCursor(0, 0);
    display1.println("No data");
    display2.setTextSize(1);
    display2.setCursor(0, 0);
    display2.println("Waiting on");
    display2.println("aggregator...");
    display1.display();
    display2.display();
    return;
  }

  char pageLabel[12];
  snprintf(pageLabel, sizeof(pageLabel), "%d/%d", currentItem + 1, itemCount);

  if (currentItem == 0) {
    // ---- host page ----
    JsonObject host = dashboardDoc["host"];

    display1.setTextSize(2);
    display1.setCursor(0, 0);
    display1.println("HOST");
    display1.setTextSize(1);
    display1.setCursor(0, 48);
    display1.println(pageLabel);

    display2.setTextSize(1);
    display2.setCursor(0, 0);
    if (!host.isNull()) {
      display2.printf("CPU  %.1f%%\n", (float)(host["cpu_percent"] | 0.0));
      display2.printf("MEM  %.1f%%\n", (float)(host["mem_percent"] | 0.0));
      display2.printf("%d/%dMB\n", (int)(host["mem_used_mb"] | 0),
                       (int)(host["mem_total_mb"] | 0));
      display2.printf("DISK %.1f%%\n", (float)(host["disk_percent"] | 0.0));
      if (!host["temp_c"].isNull()) {
        display2.printf("TEMP %.0fC\n", (float)(host["temp_c"] | 0.0));
      } else {
        display2.println("TEMP n/a");
      }
      display2.printf("UP %s\n", (const char *)(host["uptime"] | "?"));
    } else {
      display2.println("host data");
      display2.println("unavailable");
    }
  } else {
    // ---- container page ----
    JsonArray containers = dashboardDoc["containers"];
    JsonObject c = containers[currentItem - 1];
    const char *name = c["name"] | "?";
    const char *status = c["status"] | "?";
    bool running = strcmp(status, "running") == 0;

    display1.setTextSize(2);
    display1.setCursor(0, 0);
    display1.println(truncated(name, 10));
    display1.setTextSize(1);
    display1.setCursor(0, 32);
    display1.println(status);
    display1.setCursor(0, 48);
    display1.println(pageLabel);

    bool downRecently = c["down_recently"] | false;
    long downSeconds = c["down_duration_seconds"] | 0L;

    if (running) {
      display2.setTextSize(1);
      display2.setCursor(0, 0);
      display2.printf("CPU  %.1f%%\n", (float)(c["cpu_percent"] | 0.0));
      display2.printf("MEM  %d/%dMB\n", (int)(c["mem_used_mb"] | 0),
                       (int)(c["mem_total_mb"] | 0));
    } else if (downRecently) {
      // These SSD1306 panels are two-tone: roughly y0-15 is yellow, y16-63
      // is blue. Keep the warning glyph entirely in the yellow strip and
      // all text in the blue area so nothing straddles the color seam.
      const int iconSize = 14;
      const int iconX = (SCREEN_WIDTH - iconSize) / 2;
      drawWarningIcon(display2, iconX, 1, iconSize);

      display2.setTextSize(2);
      display2.setCursor(0, 18);
      display2.println("DOWN");
      display2.setTextSize(1);
      display2.setCursor(0, 40);
      display2.println("last up:");
      display2.setCursor(0, 52);
      display2.println(formatDuration(downSeconds) + " ago");
    } else {
      display2.setTextSize(1);
      display2.setCursor(0, 0);
      display2.println("(not running)");
    }
  }

  display1.display();
  display2.display();
}

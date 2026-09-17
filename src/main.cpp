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

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>  // cosf/sinf for the starfield animation

#include "secrets.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define OLED_RESET -1  // no dedicated reset pin on these modules

// Bus 1 pins (display 1, left)
#define DISP1_SDA 8
#define DISP1_SCL 9

// Bus 2 pins (display 2, right)
#define DISP2_SDA 6
#define DISP2_SCL 7

TwoWire I2CBus2 = TwoWire(1);  // second hardware I2C controller

Adafruit_SSD1306 display1(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_SSD1306 display2(SCREEN_WIDTH, SCREEN_HEIGHT, &I2CBus2, OLED_RESET);

const unsigned long FETCH_INTERVAL_MS = 15000;
const unsigned long PAGE_INTERVAL_MS = 5000;
unsigned long lastFetch = 0;
unsigned long lastPageSwitch = 0;

JsonDocument dashboardDoc;
bool dashboardValid = false;
int itemCount = 0;    // 1 (host) + number of containers
int currentItem = 0;  // 0 = host, 1..N = containers[0..N-1]

// ---- Mode / idle / screensaver state ----
enum DisplayMode { MODE_DATA, MODE_SCREENSAVER };
DisplayMode currentMode = MODE_DATA;

const unsigned long IDLE_TIMEOUT_MS = 10UL * 1000UL;  // 10 minutes
unsigned long lastActivityMs = 0;

// Redraw ~6-7x/sec -- fast enough for the animations to read as motion
// rather than a slideshow, still comfortably cheap for two mono I2C panels.
const unsigned long SCREENSAVER_TICK_MS = 150;
unsigned long lastScreensaverTick = 0;
// Real elapsed time between screensaver redraws, used to drive all motion
// by dt (px/sec) instead of a fixed px-per-tick step -- so animation speed
// stays correct regardless of the tick rate above, and a long gap (e.g.
// first frame after waking from an idle MODE_DATA stretch) doesn't jump.
unsigned long lastScreensaverUpdateMs = 0;

const int BOOT_BUTTON_PIN = 0;  // BOOT button, active LOW, needs INPUT_PULLUP
const unsigned long DEBOUNCE_MS = 50;
bool lastButtonReading = HIGH;
bool buttonStablePressed = false;
unsigned long lastButtonChangeMs = 0;
// Short tap vs. long hold, evaluated only for presses that started in
// MODE_DATA (a press that starts during the screensaver always just wakes
// it, however long it's held -- see handleButton()).
const unsigned long LONG_PRESS_MS = 800;
unsigned long buttonPressStartMs = 0;
bool longPressFired = false;
DisplayMode modeAtPressDown = MODE_DATA;

// UK time incl. automatic BST handling. Change this if the box ever moves.
const char *TZ_STRING = "GMT0BST,M3.5.0/1,M10.5.0";

// ---- Night dimming: lower OLED contrast overnight so the panels aren't
// glaring in a dark room. Window wraps past midnight (22 -> 7). ----
const int NIGHT_DIM_START_HOUR = 22;  // 10pm
const int NIGHT_DIM_END_HOUR = 7;     // 7am
bool nightDimActive = false;
const unsigned long NIGHT_CHECK_INTERVAL_MS = 30UL * 1000UL;
unsigned long lastNightCheck = 0;

// ---- Weather state ----
struct WeatherData {
  float tempC = 0;
  int weatherCode = -1;
  bool isDay = true;
  float tempMaxC = 0;
  float tempMinC = 0;
  int precipProbMax = -1;  // -1 = not available
  bool valid = false;
};
WeatherData lastWeather;

const unsigned long WEATHER_FETCH_INTERVAL_MS = 10UL * 60UL * 1000UL;  // 10 min
unsigned long lastWeatherFetch = 0;

// ---- Right-display page cycle: weather interleaved with animations ----
// STARFIELD and RAIN are purely decorative; PULSE, LOADBARS and FLEET are
// all driven by the live dashboard data (see their render functions).
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

// ---- Pong interlude: every PONG_INTERVAL_MS, takes over BOTH panels for
// PONG_DURATION_MS, treating them as one 256x64 canvas (display1 = the left
// half, display2 = the right half) -- a ball crosses the seam between them,
// bounced back by auto-tracking paddles at the two outer edges. ----
const unsigned long PONG_INTERVAL_MS = 40000;
const unsigned long PONG_DURATION_MS = 6000;
bool pongActive = false;
unsigned long pongStartMs = 0;
unsigned long lastPongTrigger = 0;

// ---- DVD-logo bounce, clock screen's yellow strip (y0-15) ----
// Float position + px/sec speed (rather than px/tick) so the pace stays the
// same no matter what SCREENSAVER_TICK_MS is -- this is the animation the
// user specifically said to keep as-is.
const int DVD_ICON_SIZE = 8;
const float DVD_SPEED = 3.0f;  // px/sec, matches the original 3px-per-1000ms pace
const int DVD_STRIP_HEIGHT = 16;
float dvdX = 20, dvdY = 4;
float dvdDirX = 1, dvdDirY = 1;
int dvdShape = 0;  // cycles to a different shape on each wall hit

// ---------------------------------------------------------------------------
// Displays
// ---------------------------------------------------------------------------

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

void drawWarningIcon(Adafruit_SSD1306 &d, int x, int y, int size) {
  d.drawTriangle(x, y + size, x + size / 2, y, x + size, y + size, SSD1306_WHITE);
  d.fillRect(x + size / 2 - 1, y + size / 3, 2, size / 3, SSD1306_WHITE);
  d.fillRect(x + size / 2 - 1, y + size - 6, 2, 2, SSD1306_WHITE);
}

// ---- Weather icons -- compact, sized to fit the ~14px-tall yellow strip ----

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

// Small animated accents, drawn in the blue zone below the main icon.
// `frame` drives slow, deliberate motion (~1 step/sec) independent of the
// screensaver's own redraw rate -- callers pass millis()/1000 so these stay
// paced the same regardless of how often renderWeatherPage() is called.

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

// ---------------------------------------------------------------------------
// Right-display animations (full-screen, shown in rotation with weather --
// see RIGHT_PAGE_SEQUENCE). All driven by dt (seconds since last screensaver
// redraw) rather than a fixed step, so speed doesn't depend on tick rate.
// ---------------------------------------------------------------------------

// ---- Starfield: points radiate outward from center, like flying through
// stars. Each resets to the center with a fresh random direction once it
// flies off an edge. ----
struct Star {
  float x, y;    // current position
  float dx, dy;  // unit direction, fixed for the star's lifetime
  float dist;    // distance travelled from center so far
};
const int STAR_COUNT = 12;
Star stars[STAR_COUNT];
bool starsInitialized = false;

void resetStar(Star &s) {
  float angle = random(0, 360) * (PI / 180.0f);
  s.dx = cosf(angle);
  s.dy = sinf(angle);
  s.dist = random(0, 6);
  s.x = SCREEN_WIDTH / 2 + s.dx * s.dist;
  s.y = SCREEN_HEIGHT / 2 + s.dy * s.dist;
}

void renderStarfield(Adafruit_SSD1306 &d, float dt) {
  if (!starsInitialized) {
    for (int i = 0; i < STAR_COUNT; i++) resetStar(stars[i]);
    starsInitialized = true;
  }
  const float STAR_SPEED = 40.0f;  // px/sec of outward travel
  for (int i = 0; i < STAR_COUNT; i++) {
    Star &s = stars[i];
    s.dist += STAR_SPEED * dt;
    s.x = SCREEN_WIDTH / 2 + s.dx * s.dist;
    s.y = SCREEN_HEIGHT / 2 + s.dy * s.dist;

    if (s.x < 0 || s.x >= SCREEN_WIDTH || s.y < 0 || s.y >= SCREEN_HEIGHT) {
      resetStar(s);
      continue;
    }

    int px = (int)s.x, py = (int)s.y;
    d.drawPixel(px, py, SSD1306_WHITE);
    if (s.dist > 20) {
      // further out = faster-looking = draw a short trailing streak
      int tx = (int)(s.x - s.dx * 2);
      int ty = (int)(s.y - s.dy * 2);
      d.drawLine(tx, ty, px, py, SSD1306_WHITE);
    }
  }
}

// Moves `current` toward `target` at `speedPerSec`, clamping exactly onto
// the target rather than overshooting it -- shared by every animation below
// that eases a displayed value toward a live data point.
void easeToward(float &current, float target, float speedPerSec, float dt) {
  float diff = target - current;
  float adiff = diff < 0 ? -diff : diff;
  float step = speedPerSec * dt;
  if (adiff < step) current = target;
  else current += (diff > 0 ? step : -step);
}

// ---- System Pulse: the old "Equalizer" bars, now driven by live stats
// instead of random targets. Bars 0-3 are host CPU / MEM / DISK / TEMP (as a
// percent of a sensible ceiling); bars 4-7 are the busiest running
// containers' CPU%, idle (near-zero) if there aren't that many running. Bar
// height still eases toward its target, so it keeps the bouncy look, it's
// just chasing real numbers now instead of random ones. ----
const int PULSE_BAR_COUNT = 8;
const int PULSE_MAX_BAR_HEIGHT = 48;  // leaves room for the caption row above
float pulseHeight[PULSE_BAR_COUNT];
float pulseTarget[PULSE_BAR_COUNT];

void updatePulseTargets() {
  float pct[PULSE_BAR_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0};

  if (dashboardValid) {
    JsonObject host = dashboardDoc["host"];
    if (!host.isNull()) {
      pct[0] = host["cpu_percent"] | 0.0;
      pct[1] = host["mem_percent"] | 0.0;
      pct[2] = host["disk_percent"] | 0.0;
      if (!host["temp_c"].isNull()) {
        float temp = host["temp_c"] | 0.0;
        pct[3] = (temp / 90.0f) * 100.0f;  // 90C treated as "full" for the bar
      }
    }

    JsonArray containers = dashboardDoc["containers"];
    if (!containers.isNull()) {
      float top[4] = {-1, -1, -1, -1};
      for (JsonObject c : containers) {
        const char *status = c["status"] | "?";
        if (strcmp(status, "running") != 0) continue;
        float cpu = c["cpu_percent"] | 0.0;
        for (int i = 0; i < 4; i++) {
          if (cpu > top[i]) {
            for (int j = 3; j > i; j--) top[j] = top[j - 1];
            top[i] = cpu;
            break;
          }
        }
      }
      for (int i = 0; i < 4; i++) {
        if (top[i] > 0) pct[4 + i] = top[i];
      }
    }
  }

  for (int i = 0; i < PULSE_BAR_COUNT; i++) {
    float clamped = pct[i] < 0 ? 0 : (pct[i] > 100 ? 100 : pct[i]);
    pulseTarget[i] = (clamped / 100.0f) * PULSE_MAX_BAR_HEIGHT;
  }
}

void renderSystemPulse(Adafruit_SSD1306 &d, float dt) {
  updatePulseTargets();  // cheap re-read of the cached dashboard each frame,
                          // so bars react the moment the next 15s fetch lands

  const int barWidth = 12;
  const int gap = 4;
  const int baseY = SCREEN_HEIGHT - 2;
  const float PULSE_SPEED = 60.0f;  // px/sec -- real values shift less often
                                     // than the old random-bounce version

  // Caption row so this doesn't read as an unlabeled bar chart, plus a
  // divider between the 4 host bars (CPU/MEM/DISK/TEMP) and the 4 busiest
  // running containers' CPU% -- the two groups otherwise look identical.
  d.setTextSize(1);
  d.setCursor(0, 0);
  d.print("HOST");
  d.setCursor(80, 0);
  d.print("TOP CPU");
  int dividerX = 4 * (barWidth + gap);
  d.drawFastVLine(dividerX, 9, baseY - 9, SSD1306_WHITE);

  for (int i = 0; i < PULSE_BAR_COUNT; i++) {
    easeToward(pulseHeight[i], pulseTarget[i], PULSE_SPEED, dt);

    int x = i * (barWidth + gap) + 2;
    int h = (int)pulseHeight[i];
    if (h < 1) h = 1;  // always show a sliver so an idle bar doesn't vanish
    d.fillRect(x, baseY - h, barWidth - 4, h, SSD1306_WHITE);
  }
}

// ---- Load Bars: two full-width horizontal progress bars for host CPU% and
// MEM%, each eased toward its live value, labeled directly above the bar
// they belong to. This replaces an earlier concentric-arc "gauge" design --
// hand-computed circles from trig just don't have enough pixels to read
// cleanly at this radius on a 128x64 1-bit panel (no anti-aliasing, and two
// close radii blur into each other). Axis-aligned rectangles stay crisp at
// any resolution, so bars are the more honest fit for a screen this small. ----
float loadBarCpuDisplay = 0, loadBarMemDisplay = 0;  // eased 0-100

void drawLoadBar(Adafruit_SSD1306 &d, int x, int y, int w, int h, const char *label,
                  float percent) {
  d.setTextSize(1);
  d.setCursor(x, y - 9);
  d.printf("%s %.0f%%", label, percent);

  d.drawRect(x, y, w, h, SSD1306_WHITE);
  int fillW = (int)((percent / 100.0f) * (w - 2));
  if (fillW > 0) d.fillRect(x + 1, y + 1, fillW, h - 2, SSD1306_WHITE);
}

void renderLoadBars(Adafruit_SSD1306 &d, float dt) {
  float targetCpu = 0, targetMem = 0;
  if (dashboardValid) {
    JsonObject host = dashboardDoc["host"];
    if (!host.isNull()) {
      targetCpu = host["cpu_percent"] | 0.0;
      targetMem = host["mem_percent"] | 0.0;
    }
  }

  const float LOAD_BAR_SPEED = 60.0f;  // %/sec
  easeToward(loadBarCpuDisplay, targetCpu, LOAD_BAR_SPEED, dt);
  easeToward(loadBarMemDisplay, targetMem, LOAD_BAR_SPEED, dt);

  const int barW = 110, barH = 14;
  const int x = (SCREEN_WIDTH - barW) / 2;
  drawLoadBar(d, x, 18, barW, barH, "CPU", loadBarCpuDisplay);
  drawLoadBar(d, x, 44, barW, barH, "MEM", loadBarMemDisplay);
}

// ---- Matrix rain: independent falling columns of random characters,
// respawning at the top with a new speed/char once they fall off. ----
const int RAIN_COL_COUNT = 16;  // 128px / 8px-per-column
float rainY[RAIN_COL_COUNT];
float rainSpeed[RAIN_COL_COUNT];
char rainChar[RAIN_COL_COUNT];
bool rainInitialized = false;

char randomRainChar() {
  static const char chars[] = "01$#%&*+=<>?";
  return chars[random(0, (int)(sizeof(chars) - 1))];
}

void renderMatrixRain(Adafruit_SSD1306 &d, float dt) {
  if (!rainInitialized) {
    for (int i = 0; i < RAIN_COL_COUNT; i++) {
      rainY[i] = random(-64, 0);
      rainSpeed[i] = 20.0f + random(0, 40);
      rainChar[i] = randomRainChar();
    }
    rainInitialized = true;
  }

  d.setTextSize(1);
  for (int i = 0; i < RAIN_COL_COUNT; i++) {
    rainY[i] += rainSpeed[i] * dt;
    if (rainY[i] > SCREEN_HEIGHT) {
      rainY[i] = random(-40, -8);
      rainSpeed[i] = 20.0f + random(0, 40);
      rainChar[i] = randomRainChar();
    }
    // occasional flicker to a different char, like the movie effect
    if (random(0, 20) == 0) rainChar[i] = randomRainChar();

    d.setCursor(i * 8, (int)rainY[i]);
    d.print(rainChar[i]);
  }
}

// ---- Pong interlude: draws directly on both display1 and display2 (rather
// than taking a single `d` like the other animations) since the whole point
// is treating them as one continuous canvas. Canvas x from 0 up to
// SCREEN_WIDTH is display1, SCREEN_WIDTH up to 2*SCREEN_WIDTH is display2.
// No scoring -- the paddles always intercept, it's meant to be watched, not
// played. ----
const int PONG_CANVAS_WIDTH = SCREEN_WIDTH * 2;
const int PONG_BALL_SIZE = 4;
const int PONG_PADDLE_HEIGHT = 16;
const int PONG_PADDLE_WIDTH = 3;
const int PONG_PADDLE_MARGIN = 2;  // gap between paddle and the outer edge
float pongBallX = PONG_CANVAS_WIDTH / 2.0f, pongBallY = SCREEN_HEIGHT / 2.0f;
float pongBallDX = 70.0f, pongBallDY = 50.0f;  // px/sec
float pongLeftPaddleY = SCREEN_HEIGHT / 2.0f - PONG_PADDLE_HEIGHT / 2.0f;
float pongRightPaddleY = SCREEN_HEIGHT / 2.0f - PONG_PADDLE_HEIGHT / 2.0f;

void renderPong(float dt) {
  pongBallX += pongBallDX * dt;
  pongBallY += pongBallDY * dt;

  if (pongBallY <= 0) {
    pongBallY = 0;
    pongBallDY = fabsf(pongBallDY);
  } else if (pongBallY >= SCREEN_HEIGHT - PONG_BALL_SIZE) {
    pongBallY = SCREEN_HEIGHT - PONG_BALL_SIZE;
    pongBallDY = -fabsf(pongBallDY);
  }

  const int leftPaddleX = PONG_PADDLE_MARGIN;
  const int rightPaddleX = PONG_CANVAS_WIDTH - PONG_PADDLE_MARGIN - PONG_PADDLE_WIDTH;
  if (pongBallDX < 0 && pongBallX <= leftPaddleX + PONG_PADDLE_WIDTH) {
    pongBallX = leftPaddleX + PONG_PADDLE_WIDTH;
    pongBallDX = fabsf(pongBallDX);
  } else if (pongBallDX > 0 && pongBallX >= rightPaddleX - PONG_BALL_SIZE) {
    pongBallX = rightPaddleX - PONG_BALL_SIZE;
    pongBallDX = -fabsf(pongBallDX);
  }

  // Paddles ease toward the ball's y at a finite speed rather than snapping
  // to it, so they read as "tracking" rather than teleporting.
  const float PADDLE_SPEED = 60.0f;  // px/sec
  float target = pongBallY - PONG_PADDLE_HEIGHT / 2.0f;
  float maxStep = PADDLE_SPEED * dt;

  float leftDiff = target - pongLeftPaddleY;
  float leftStep = leftDiff < -maxStep ? -maxStep : (leftDiff > maxStep ? maxStep : leftDiff);
  pongLeftPaddleY += leftStep;

  float rightDiff = target - pongRightPaddleY;
  float rightStep = rightDiff < -maxStep ? -maxStep : (rightDiff > maxStep ? maxStep : rightDiff);
  pongRightPaddleY += rightStep;

  if (pongLeftPaddleY < 0) pongLeftPaddleY = 0;
  if (pongLeftPaddleY > SCREEN_HEIGHT - PONG_PADDLE_HEIGHT) pongLeftPaddleY = SCREEN_HEIGHT - PONG_PADDLE_HEIGHT;
  if (pongRightPaddleY < 0) pongRightPaddleY = 0;
  if (pongRightPaddleY > SCREEN_HEIGHT - PONG_PADDLE_HEIGHT) pongRightPaddleY = SCREEN_HEIGHT - PONG_PADDLE_HEIGHT;

  display1.fillRect(leftPaddleX, (int)pongLeftPaddleY, PONG_PADDLE_WIDTH, PONG_PADDLE_HEIGHT,
                     SSD1306_WHITE);
  display2.fillRect(rightPaddleX - SCREEN_WIDTH, (int)pongRightPaddleY, PONG_PADDLE_WIDTH,
                     PONG_PADDLE_HEIGHT, SSD1306_WHITE);

  // Dotted center-court line right at the seam, so it reads as one court
  // split across two panels rather than two unrelated screens.
  for (int y = 0; y < SCREEN_HEIGHT; y += 6) {
    display1.drawPixel(SCREEN_WIDTH - 1, y, SSD1306_WHITE);
    display2.drawPixel(0, y, SSD1306_WHITE);
  }

  if (pongBallX < SCREEN_WIDTH) {
    display1.fillRect((int)pongBallX, (int)pongBallY, PONG_BALL_SIZE, PONG_BALL_SIZE, SSD1306_WHITE);
  } else {
    display2.fillRect((int)pongBallX - SCREEN_WIDTH, (int)pongBallY, PONG_BALL_SIZE, PONG_BALL_SIZE,
                       SSD1306_WHITE);
  }
}

// ---- Fleet Grid: one cell per container -- solid = running, blinking
// hollow = down_recently, dim outline = stopped. An always-on "server room"
// overview rather than a single stat, so it stays interesting to glance at
// even when nothing is down. Shows the first FLEET_GRID_COLS*FLEET_GRID_ROWS
// containers if there are more than that. ----
const int FLEET_GRID_COLS = 5;
const int FLEET_GRID_ROWS = 4;

void renderFleetGrid(Adafruit_SSD1306 &d, float dt) {
  (void)dt;  // layout only reacts to data, not to elapsed time

  if (!dashboardValid) {
    d.setTextSize(1);
    d.setCursor(0, 20);
    d.println("No fleet data");
    return;
  }

  JsonArray containers = dashboardDoc["containers"];
  int count = containers.isNull() ? 0 : containers.size();
  int maxCells = FLEET_GRID_COLS * FLEET_GRID_ROWS;
  if (count > maxCells) count = maxCells;

  const int cellW = SCREEN_WIDTH / FLEET_GRID_COLS;
  const int cellH = SCREEN_HEIGHT / FLEET_GRID_ROWS;
  const int pad = 2;
  bool blinkOn = (millis() % 900) < 600;  // same cadence as the full alert page

  for (int i = 0; i < count; i++) {
    JsonObject c = containers[i];
    const char *status = c["status"] | "?";
    bool running = strcmp(status, "running") == 0;
    bool down = c["down_recently"] | false;

    int col = i % FLEET_GRID_COLS;
    int row = i / FLEET_GRID_COLS;
    int x = col * cellW + pad;
    int y = row * cellH + pad;
    int w = cellW - pad * 2;
    int h = cellH - pad * 2;

    if (down) {
      if (blinkOn) d.drawRect(x, y, w, h, SSD1306_WHITE);
    } else if (running) {
      d.fillRect(x, y, w, h, SSD1306_WHITE);
    } else {
      d.drawRect(x, y, w, h, SSD1306_WHITE);
    }
  }
}

// Left screen: big label for whatever's currently shown + a page indicator.
// Right screen: that item's actual numbers.
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

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

void connectWiFi() {
  showStatus("Connecting WiFi", WIFI_SSID);
  Serial.printf("Connecting to WiFi SSID '%s'", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nWiFi connect timed out, retrying...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      start = millis();
    }
  }

  Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
  showStatus("WiFi connected", WiFi.localIP().toString());
}

void ensureWiFiConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi dropped, reconnecting...");
    connectWiFi();
  }
}

// ---------------------------------------------------------------------------
// Time (NTP)
// ---------------------------------------------------------------------------

bool syncTime() {
  showStatus("Syncing time", "via NTP...");
  configTzTime(TZ_STRING, "pool.ntp.org", "time.google.com");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 15000)) {
    Serial.println("NTP sync timed out -- will keep retrying in the "
                    "background, screensaver clock may be blank til then.");
    return false;
  }

  Serial.printf("Time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return true;
}

bool isNightHour(int hour) {
  if (NIGHT_DIM_START_HOUR > NIGHT_DIM_END_HOUR) {
    // window wraps past midnight, e.g. 22 -> 7
    return hour >= NIGHT_DIM_START_HOUR || hour < NIGHT_DIM_END_HOUR;
  }
  return hour >= NIGHT_DIM_START_HOUR && hour < NIGHT_DIM_END_HOUR;
}

// Checked periodically from loop() regardless of mode, so both panels dim
// whether the box is showing live data or the screensaver. Only touches the
// displays' contrast on an actual day/night transition, not every check.
void updateNightDimming() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return;  // no time yet -- leave brightness as-is

  bool night = isNightHour(timeinfo.tm_hour);
  if (night != nightDimActive) {
    nightDimActive = night;
    display1.dim(nightDimActive);
    display2.dim(nightDimActive);
    Serial.printf("Night dimming %s (hour=%d)\n", nightDimActive ? "ON" : "OFF",
                  timeinfo.tm_hour);
  }
}

// ---------------------------------------------------------------------------
// BOOT button (idle detection, screensaver wake, tap/hold actions)
// ---------------------------------------------------------------------------

// Defined further down (Dashboard fetch section) -- forward-declared here so
// forceRefresh() can call them.
bool fetchDashboard();
void logDashboard();

void onButtonPressed() {
  lastActivityMs = millis();
  if (currentMode == MODE_SCREENSAVER) {
    Serial.println("BOOT pressed -- waking from screensaver");
    currentMode = MODE_DATA;
    renderCurrentItem();
  }
}

// Short tap while awake: manually skip to the next page (same as the
// automatic page-advance, just user-triggered).
void skipToNextPage() {
  if (dashboardValid && itemCount > 0) {
    lastPageSwitch = millis();
    currentItem = (currentItem + 1) % itemCount;
    renderCurrentItem();
  }
}

// Long hold while awake: force an immediate refresh rather than waiting for
// the next FETCH_INTERVAL_MS tick.
void forceRefresh() {
  Serial.println("BOOT held -- forcing dashboard refresh");
  lastFetch = millis();
  if (fetchDashboard()) {
    dashboardValid = true;
    logDashboard();
    if (currentMode == MODE_DATA) renderCurrentItem();
  }
}

void handleButton() {
  bool reading = digitalRead(BOOT_BUTTON_PIN);
  if (reading != lastButtonReading) {
    lastButtonChangeMs = millis();
    lastButtonReading = reading;
  }

  if (millis() - lastButtonChangeMs > DEBOUNCE_MS) {
    bool pressedNow = (reading == LOW);

    if (pressedNow && !buttonStablePressed) {
      // Just pressed. Capture the mode *before* onButtonPressed() can change
      // it, so a press that starts during the screensaver is only ever a
      // wake -- never also a skip/refresh, however long it's held.
      buttonPressStartMs = millis();
      longPressFired = false;
      modeAtPressDown = currentMode;
      onButtonPressed();
    } else if (pressedNow && buttonStablePressed && !longPressFired &&
               modeAtPressDown == MODE_DATA &&
               millis() - buttonPressStartMs >= LONG_PRESS_MS) {
      // Still held past the threshold, and it started in data mode -- fire
      // the long-press action exactly once for this hold.
      longPressFired = true;
      forceRefresh();
    } else if (!pressedNow && buttonStablePressed) {
      // Just released. If it never reached the long-press threshold and
      // started in data mode, treat it as a short tap.
      if (!longPressFired && modeAtPressDown == MODE_DATA) {
        skipToNextPage();
      }
    }

    buttonStablePressed = pressedNow;
  }
}

// ---------------------------------------------------------------------------
// Weather (Open-Meteo)
// ---------------------------------------------------------------------------

bool fetchWeather(WeatherData &out) {
  WiFiClientSecure client;
  client.setInsecure();  // see file header note on this tradeoff

  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") +
               WEATHER_LAT + "&longitude=" + WEATHER_LON +
               "&current=temperature_2m,weather_code,is_day"
               "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
               "&timezone=auto";

  HTTPClient https;
  if (!https.begin(client, url)) {
    Serial.println("Weather: https.begin() failed");
    return false;
  }
  https.setTimeout(8000);
  // Open-Meteo responds with chunked transfer encoding, which
  // HTTPClient::getStream() doesn't dechunk -- deserializeJson ends up
  // reading chunk-size markers as if they were JSON. Forcing HTTP/1.0
  // avoids chunked encoding entirely (ArduinoJson's own recommended fix).
  https.useHTTP10(true);

  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Weather GET failed, HTTP code %d\n", code);
    https.end();
    return false;
  }

  JsonDocument doc;  // small, separate from dashboardDoc
  DeserializationError err = deserializeJson(doc, https.getStream());
  https.end();
  if (err) {
    Serial.printf("Weather JSON parse failed: %s\n", err.c_str());
    return false;
  }

  JsonObject current = doc["current"];
  if (current.isNull()) {
    Serial.println("Weather: no 'current' block in response");
    return false;
  }

  out.tempC = current["temperature_2m"] | 0.0;
  out.weatherCode = current["weather_code"] | -1;
  out.isDay = (current["is_day"] | 1) == 1;

  // Daily forecast: parallel arrays, index 0 is today.
  JsonObject daily = doc["daily"];
  if (!daily.isNull()) {
    JsonArray tmax = daily["temperature_2m_max"];
    JsonArray tmin = daily["temperature_2m_min"];
    JsonArray precip = daily["precipitation_probability_max"];
    if (tmax.size() > 0) out.tempMaxC = tmax[0];
    if (tmin.size() > 0) out.tempMinC = tmin[0];
    if (precip.size() > 0) out.precipProbMax = precip[0];
  }

  out.valid = true;
  return true;
}

// ---------------------------------------------------------------------------
// Screensaver
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Dashboard fetch
// ---------------------------------------------------------------------------

bool fetchDashboard() {
  HTTPClient http;
  http.begin(DASHBOARD_URL);
  http.addHeader("X-API-Key", DASHBOARD_API_KEY);
  http.setTimeout(5000);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("GET %s failed, HTTP code %d\n", DASHBOARD_URL, code);
    http.end();
    return false;
  }

  dashboardDoc.clear();
  DeserializationError err = deserializeJson(dashboardDoc, http.getStream());
  http.end();

  if (err) {
    Serial.printf("JSON parse failed: %s\n", err.c_str());
    return false;
  }

  JsonArray containers = dashboardDoc["containers"];
  itemCount = 1 + (containers.isNull() ? 0 : containers.size());

  // Keep cycling from wherever we were -- only clamp if the list shrank
  // (a container got removed) so currentItem can't point past the end.
  currentItem = (itemCount > 0) ? (currentItem % itemCount) : 0;
  return true;
}

void logDashboard() {
  JsonObject host = dashboardDoc["host"];
  JsonArray containers = dashboardDoc["containers"];
  Serial.printf("Fetched OK: host=%s containers=%u\n",
                host.isNull() ? "missing" : "ok",
                containers.isNull() ? 0 : containers.size());
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1500);  // give native-USB serial a moment to enumerate
  Serial.println("\nDashboard firmware starting");

  Wire.begin(DISP1_SDA, DISP1_SCL);
  I2CBus2.begin(DISP2_SDA, DISP2_SCL);

  bool ok1 = bringUpDisplay(display1, "Display 1");
  bool ok2 = bringUpDisplay(display2, "Display 2");
  if (!(ok1 && ok2)) {
    Serial.println("At least one display did not respond -- check SDA/SCL "
                    "wiring, power, and that both modules are on 0x3C.");
  }
  delay(1000);

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
      Serial.println("Idle timeout -- entering screensaver");
      currentMode = MODE_SCREENSAVER;
      lastScreensaverTick = 0;      // force an immediate draw
      lastScreensaverUpdateMs = 0;  // dt starts fresh, no bogus first-frame jump
      rightPageIndex = 0;           // always wake into weather, not mid-animation
      lastRightPageSwitch = 0;
      pongActive = false;           // Pong interlude timer restarts fresh too
      lastPongTrigger = millis();
    } else if (dashboardValid && itemCount > 0 &&
               millis() - lastPageSwitch >= PAGE_INTERVAL_MS) {
      lastPageSwitch = millis();
      currentItem = (currentItem + 1) % itemCount;
      renderCurrentItem();
    }
  } else {  // MODE_SCREENSAVER
    if (millis() - lastScreensaverTick >= SCREENSAVER_TICK_MS) {
      lastScreensaverTick = millis();
      renderScreensaver();
    }
  }
}
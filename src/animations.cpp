#include "animations.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>  // cosf/sinf for the starfield, fabsf for Pong
#include <string.h>

#include "config.h"
#include "dashboard.h"
#include "display_hw.h"
#include "draw_helpers.h"

namespace {

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

}  // namespace

// ---- Starfield: points radiate outward from center, like flying through
// stars. Each resets to the center with a fresh random direction once it
// flies off an edge. ----
namespace {
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
}  // namespace

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

// ---- System Pulse: the old "Equalizer" bars, now driven by live stats
// instead of random targets. Bars 0-3 are host CPU / MEM / DISK / TEMP (as a
// percent of a sensible ceiling); bars 4-7 are the busiest running
// containers' CPU%, idle (near-zero) if there aren't that many running. Bar
// height still eases toward its target, so it keeps the bouncy look, it's
// just chasing real numbers now instead of random ones. ----
namespace {
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
}  // namespace

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
// they belong to, plus a network throughput line underneath. This replaces
// an earlier concentric-arc "gauge" design -- hand-computed circles from
// trig just don't have enough pixels to read cleanly at this radius on a
// 128x64 1-bit panel (no anti-aliasing, and two close radii blur into each
// other). Axis-aligned rectangles stay crisp at any resolution, so bars are
// the more honest fit for a screen this small. ----
namespace {
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
}  // namespace

void renderLoadBars(Adafruit_SSD1306 &d, float dt) {
  float targetCpu = 0, targetMem = 0;
  float netRx = -1, netTx = -1;  // -1 = not available from this aggregator
  if (dashboardValid) {
    JsonObject host = dashboardDoc["host"];
    if (!host.isNull()) {
      targetCpu = host["cpu_percent"] | 0.0;
      targetMem = host["mem_percent"] | 0.0;
      if (!host["network_rx_bytes_per_sec"].isNull()) {
        netRx = host["network_rx_bytes_per_sec"] | 0.0;
        netTx = host["network_tx_bytes_per_sec"] | 0.0;
      }
    }
  }

  const float LOAD_BAR_SPEED = 60.0f;  // %/sec
  easeToward(loadBarCpuDisplay, targetCpu, LOAD_BAR_SPEED, dt);
  easeToward(loadBarMemDisplay, targetMem, LOAD_BAR_SPEED, dt);

  const int barW = 110, barH = 12;
  const int x = (SCREEN_WIDTH - barW) / 2;
  drawLoadBar(d, x, 9, barW, barH, "CPU", loadBarCpuDisplay);
  drawLoadBar(d, x, 33, barW, barH, "MEM", loadBarMemDisplay);

  d.setTextSize(1);
  d.setCursor(x, 52);
  if (netRx >= 0) {
    d.print("R:");
    d.print(formatRate(netRx));
    d.print("/s T:");
    d.print(formatRate(netTx));
    d.print("/s");
  } else {
    d.print("NET n/a");
  }
}

// ---- Matrix rain: independent falling columns of random characters,
// respawning at the top with a new speed/char once they fall off. ----
namespace {
const int RAIN_COL_COUNT = 16;  // 128px / 8px-per-column
float rainY[RAIN_COL_COUNT];
float rainSpeed[RAIN_COL_COUNT];
char rainChar[RAIN_COL_COUNT];
bool rainInitialized = false;

char randomRainChar() {
  static const char chars[] = "01$#%&*+=<>?";
  return chars[random(0, (int)(sizeof(chars) - 1))];
}
}  // namespace

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
namespace {
const int PONG_CANVAS_WIDTH = SCREEN_WIDTH * 2;
const int PONG_BALL_SIZE = 4;
const int PONG_PADDLE_HEIGHT = 16;
const int PONG_PADDLE_WIDTH = 3;
const int PONG_PADDLE_MARGIN = 2;  // gap between paddle and the outer edge
float pongBallX = PONG_CANVAS_WIDTH / 2.0f, pongBallY = SCREEN_HEIGHT / 2.0f;
float pongBallDX = 70.0f, pongBallDY = 50.0f;  // px/sec
float pongLeftPaddleY = SCREEN_HEIGHT / 2.0f - PONG_PADDLE_HEIGHT / 2.0f;
float pongRightPaddleY = SCREEN_HEIGHT / 2.0f - PONG_PADDLE_HEIGHT / 2.0f;
}  // namespace

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
namespace {
const int FLEET_GRID_COLS = 5;
const int FLEET_GRID_ROWS = 4;
}  // namespace

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

// ---- Trend: the only page that isn't just the current instant -- draws a
// small line chart through the aggregator's recent host_history samples, so
// you can tell "climbing" from "steady" from "just spiked" at a glance
// instead of only ever seeing a single snapshot. Scaled to the min/max
// within the visible window, not a fixed 0-100 range, so a quiet host isn't
// a flat line pinned at the bottom. ----
namespace {
void drawSparkline(Adafruit_SSD1306 &d, int x, int y, int w, int h, JsonArray values) {
  int n = values.size();
  if (n < 2) return;

  float minV = values[0];
  float maxV = values[0];
  for (JsonVariant v : values) {
    float f = v.as<float>();
    if (f < minV) minV = f;
    if (f > maxV) maxV = f;
  }
  float range = maxV - minV;
  if (range < 1.0f) range = 1.0f;  // avoid a divide-by-zero flatline

  int prevX = x;
  int prevY = y + h - (int)(((float)values[0].as<float>() - minV) / range * h);
  for (int i = 1; i < n; i++) {
    int px = x + (int)((float)i / (n - 1) * w);
    float v = values[i].as<float>();
    int py = y + h - (int)((v - minV) / range * h);
    d.drawLine(prevX, prevY, px, py, SSD1306_WHITE);
    prevX = px;
    prevY = py;
  }
}
}  // namespace

void renderTrend(Adafruit_SSD1306 &d, float dt) {
  (void)dt;  // driven entirely by the aggregator's history, not local motion

  JsonObject history;
  if (dashboardValid) history = dashboardDoc["host_history"];
  JsonArray cpuHist = history["cpu_percent"];
  JsonArray memHist = history["mem_percent"];

  if (history.isNull() || cpuHist.size() < 2) {
    d.setTextSize(1);
    d.setCursor(0, 20);
    d.println("Not enough");
    d.println("trend data yet");
    return;
  }

  d.setTextSize(1);
  d.setCursor(0, 0);
  d.print("CPU trend");
  drawSparkline(d, 0, 9, SCREEN_WIDTH, 18, cpuHist);

  d.setCursor(0, 32);
  d.print("MEM trend");
  drawSparkline(d, 0, 41, SCREEN_WIDTH, 18, memHist);
}

#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------------------------------------------------------------------
// Full-screen screensaver animations, shown in rotation with weather (see
// screensaver.cpp's RIGHT_PAGE_SEQUENCE). All driven by dt (seconds since
// last screensaver redraw) rather than a fixed step, so speed doesn't depend
// on tick rate. STARFIELD and RAIN are purely decorative; PULSE, LOADBARS
// and FLEET are all driven by the live dashboard data.
// ---------------------------------------------------------------------------

void renderStarfield(Adafruit_SSD1306 &d, float dt);
void renderSystemPulse(Adafruit_SSD1306 &d, float dt);
void renderLoadBars(Adafruit_SSD1306 &d, float dt);
void renderMatrixRain(Adafruit_SSD1306 &d, float dt);
void renderFleetGrid(Adafruit_SSD1306 &d, float dt);

// The only page that isn't just the current instant -- draws a small line
// chart through the aggregator's recent host_history samples, so you can
// tell "climbing" from "steady" from "just spiked" at a glance.
void renderTrend(Adafruit_SSD1306 &d, float dt);

// ---- Pong interlude: takes over BOTH panels, treating them as one 256x64
// canvas (display1 = the left half, display2 = the right half) -- a ball
// crosses the seam between them, bounced back by auto-tracking paddles at
// the two outer edges. Draws directly on display1/display2 (rather than
// taking a single `d` like the other animations) since the whole point is
// treating them as one continuous canvas. ----
void renderPong(float dt);

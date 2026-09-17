#pragma once

// ---- Display panels ----
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

// ---- BOOT button ----
const int BOOT_BUTTON_PIN = 0;  // active LOW, needs INPUT_PULLUP
const unsigned long DEBOUNCE_MS = 50;
const unsigned long LONG_PRESS_MS = 800;

// ---- Data-mode cycling: two independent timers, decoupled so the display
// keeps cycling smoothly even between fetches ----
const unsigned long FETCH_INTERVAL_MS = 15000;
const unsigned long PAGE_INTERVAL_MS = 5000;

// ---- Idle / screensaver ----
const unsigned long IDLE_TIMEOUT_MS = 10UL * 60UL * 1000UL;  // 10 minutes
// Redraw ~6-7x/sec -- fast enough for the animations to read as motion
// rather than a slideshow, still comfortably cheap for two mono I2C panels.
const unsigned long SCREENSAVER_TICK_MS = 150;

// ---- Time zone: UK incl. automatic BST handling. Change this if the box
// ever moves. ----
const char *const TZ_STRING = "GMT0BST,M3.5.0/1,M10.5.0";

// ---- Dimming: lower OLED contrast overnight (scheduled) or on demand
// (hold BOOT from the screensaver) so the panels aren't glaring in a dark
// room either way -- see time_sync.h. Night window wraps past midnight
// (22 -> 7). ----
const int NIGHT_DIM_START_HOUR = 22;  // 10pm
const int NIGHT_DIM_END_HOUR = 7;     // 7am
const unsigned long NIGHT_CHECK_INTERVAL_MS = 30UL * 1000UL;

// ---- Weather ----
const unsigned long WEATHER_FETCH_INTERVAL_MS = 10UL * 60UL * 1000UL;  // 10 min
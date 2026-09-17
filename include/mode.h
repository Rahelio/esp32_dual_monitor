#pragma once

// ---- Mode / idle / screensaver state ----
enum DisplayMode { MODE_DATA, MODE_SCREENSAVER };

extern DisplayMode currentMode;
extern unsigned long lastActivityMs;

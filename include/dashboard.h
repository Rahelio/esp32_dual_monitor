#pragma once

#include <ArduinoJson.h>

extern JsonDocument dashboardDoc;
extern bool dashboardValid;
extern int itemCount;    // 1 (host) + number of containers
extern int currentItem;  // 0 = host, 1..N = containers[0..N-1]

// Scheduling for the two independent data-mode timers: how often fresh data
// is pulled from the aggregator, and how often the shown item advances.
// Exposed here (rather than kept file-local) because button.cpp's manual
// skip/refresh actions need to reset them.
extern unsigned long lastFetch;
extern unsigned long lastPageSwitch;

bool fetchDashboard();
void logDashboard();

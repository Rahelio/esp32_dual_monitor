#include "time_sync.h"

#include <Arduino.h>
#include <time.h>

#include "config.h"
#include "display_hw.h"

namespace {
bool nightDimActive = false;
bool manualMuteActive = false;
bool dimApplied = false;  // what's actually been sent to the displays so far

bool isNightHour(int hour) {
  if (NIGHT_DIM_START_HOUR > NIGHT_DIM_END_HOUR) {
    // window wraps past midnight, e.g. 22 -> 7
    return hour >= NIGHT_DIM_START_HOUR || hour < NIGHT_DIM_END_HOUR;
  }
  return hour >= NIGHT_DIM_START_HOUR && hour < NIGHT_DIM_END_HOUR;
}

// Combines the schedule and the on-demand override into one applied state --
// dim if EITHER wants it dim -- and only touches the displays' contrast on
// an actual change, not every check.
void applyDimState() {
  bool shouldDim = nightDimActive || manualMuteActive;
  if (shouldDim != dimApplied) {
    dimApplied = shouldDim;
    display1.dim(dimApplied);
    display2.dim(dimApplied);
    Serial.printf("Display dim %s (night=%d, manual=%d)\n", dimApplied ? "ON" : "OFF",
                  nightDimActive, manualMuteActive);
  }
}
}  // namespace

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

void updateNightDimming() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return;  // no time yet -- leave brightness as-is
  nightDimActive = isNightHour(timeinfo.tm_hour);
  applyDimState();
}

void toggleManualDim() {
  manualMuteActive = !manualMuteActive;
  applyDimState();
}

void clearManualDim() {
  manualMuteActive = false;
  applyDimState();
}

bool isManualDimActive() { return manualMuteActive; }

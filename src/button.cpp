#include "button.h"

#include <Arduino.h>

#include "config.h"
#include "dashboard.h"
#include "dashboard_view.h"
#include "mode.h"
#include "screensaver.h"
#include "time_sync.h"

namespace {

bool lastButtonReading = HIGH;
bool buttonStablePressed = false;
unsigned long lastButtonChangeMs = 0;
// Short tap vs. long hold, evaluated per press against whichever mode it
// started in -- a press that starts during the screensaver always wakes it
// first (see onButtonPressed()), but modeAtPressDown remembers where it
// really started so the eventual tap/hold action still matches that.
unsigned long buttonPressStartMs = 0;
bool longPressFired = false;
DisplayMode modeAtPressDown = MODE_DATA;

void onButtonPressed() {
  lastActivityMs = millis();
  if (currentMode == MODE_SCREENSAVER) {
    Serial.println("BOOT pressed -- waking from screensaver");
    currentMode = MODE_DATA;
    renderCurrentItem();
    // The manual dim override is deliberately left alone here -- this wake
    // is still optimistic (the press might turn into a long hold instead,
    // see toggleManualMute()). It's only cleared once handleButton()
    // confirms this was a genuine short tap, in the release branch below.
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

// Long hold that started during the screensaver: toggle a manual dim
// override (e.g. for a meeting) independent of the night-dim schedule, and
// undo the optimistic wake onButtonPressed() already did for this press --
// holding from asleep should mute it, not wake it.
void toggleManualMute() {
  toggleManualDim();
  Serial.printf("BOOT held from screensaver -- manual mute/dim %s\n",
                isManualDimActive() ? "ON" : "OFF");
  currentMode = MODE_SCREENSAVER;
  forceImmediateRedraw();  // at the new brightness
}

}  // namespace

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
      // wake or a mute-toggle -- never also a skip/refresh, however long
      // it's held.
      buttonPressStartMs = millis();
      longPressFired = false;
      modeAtPressDown = currentMode;
      onButtonPressed();
    } else if (pressedNow && buttonStablePressed && !longPressFired &&
               millis() - buttonPressStartMs >= LONG_PRESS_MS) {
      // Still held past the threshold -- fire the long-press action for
      // whichever mode this press started in, exactly once per hold.
      longPressFired = true;
      if (modeAtPressDown == MODE_DATA) {
        forceRefresh();
      } else {
        toggleManualMute();
      }
    } else if (!pressedNow && buttonStablePressed) {
      // Just released. If it never reached the long-press threshold, treat
      // it as a short tap -- what that means depends on where it started.
      if (!longPressFired) {
        if (modeAtPressDown == MODE_DATA) {
          skipToNextPage();
        } else if (isManualDimActive()) {
          // Confirmed short tap that woke the device out of a muted
          // screensaver -- a real wake always means full brightness.
          clearManualDim();
        }
      }
    }

    buttonStablePressed = pressedNow;
  }
}

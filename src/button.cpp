#include "button.h"

#include <Arduino.h>

#include "config.h"
#include "dashboard.h"
#include "dashboard_view.h"
#include "mode.h"

namespace {

bool lastButtonReading = HIGH;
bool buttonStablePressed = false;
unsigned long lastButtonChangeMs = 0;
// Short tap vs. long hold, evaluated only for presses that started in
// MODE_DATA (a press that starts during the screensaver always just wakes
// it, however long it's held -- see handleButton()).
unsigned long buttonPressStartMs = 0;
bool longPressFired = false;
DisplayMode modeAtPressDown = MODE_DATA;

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

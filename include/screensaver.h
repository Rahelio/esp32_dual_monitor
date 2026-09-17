#pragma once

// Resets all screensaver-internal timers/state and switches currentMode to
// MODE_SCREENSAVER. Call this exactly on the idle-timeout transition into
// the screensaver, so nothing carries over stale from the previous session
// (e.g. mid-animation, or a half-elapsed Pong interlude).
void enterScreensaver();

// Draws one frame of the idle screensaver -- a live clock (NTP-synced,
// DST-aware) with a bouncing DVD-logo icon on the left, and on the right a
// repeating cycle of weather interleaved with full-screen animations. Every
// so often both panels are taken over for a Pong interlude. A container
// that's gone down recently pre-empts all of that with a blinking warning
// on both panels. See main.cpp's header comment for the full picture.
void renderScreensaver();

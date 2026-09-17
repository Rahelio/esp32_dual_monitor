#pragma once

// Resets all screensaver-internal timers/state and switches currentMode to
// MODE_SCREENSAVER. Call this exactly on the idle-timeout transition into
// the screensaver, so nothing carries over stale from the previous session
// (e.g. mid-animation, or a half-elapsed Pong interlude).
void enterScreensaver();

// True once at least SCREENSAVER_TICK_MS has elapsed since the screensaver
// was last redrawn (or since forceImmediateRedraw() was last called). Call
// renderScreensaver() only when this returns true -- it also resets the
// internal clock, so call it at most once per loop() iteration.
bool screensaverTickDue();

// Forces the next screensaverTickDue() call to return true. Used when
// something changes that the screensaver should reflect right away (e.g.
// toggling the manual dim override) rather than waiting out the tick.
void forceImmediateRedraw();

// Draws one frame of the idle screensaver -- a live clock (NTP-synced,
// DST-aware) with a bouncing DVD-logo icon on the left, and on the right a
// repeating cycle of weather interleaved with full-screen animations. Every
// so often both panels are taken over for a Pong interlude. A container
// that's gone down recently pre-empts all of that with a blinking warning
// on both panels. See main.cpp's header comment for the full picture.
void renderScreensaver();

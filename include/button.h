#pragma once

// Debounces the BOOT button and dispatches its short-tap / long-hold
// actions (skip page / wake screensaver / force refresh). Must be polled
// every loop() iteration.
void handleButton();

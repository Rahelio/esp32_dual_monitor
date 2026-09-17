#pragma once

// Reboots the board if the main loop ever stops feeding it (a WiFi-library
// hang is the realistic scenario on this hardware). Call once from setup()
// before anything that could conceivably hang. Feed it via
// esp_task_wdt_reset() once per loop() iteration, and also inside any
// blocking retry loop (e.g. WiFi reconnect) that's expected to legitimately
// run for a while, so that doesn't itself get mistaken for a hang.
void setupWatchdog();

#pragma once

bool syncTime();

// Checked periodically from loop() regardless of mode, so both panels dim
// whether the box is showing live data or the screensaver. Only touches the
// displays' contrast on an actual day/night transition, not every check.
void updateNightDimming();

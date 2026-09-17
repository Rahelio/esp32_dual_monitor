#pragma once

bool syncTime();

// Checked periodically from loop() regardless of mode, so both panels dim
// whether the box is showing live data or the screensaver.
void updateNightDimming();

// ---- Manual dim override (e.g. for a meeting), independent of the night
// schedule -- combined with it so either alone dims the panels and neither
// silently undoes the other. ----
void toggleManualDim();
void clearManualDim();
bool isManualDimActive();

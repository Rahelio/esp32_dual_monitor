#include "dashboard.h"

#include <Arduino.h>
#include <HTTPClient.h>

#include "secrets.h"

JsonDocument dashboardDoc;
bool dashboardValid = false;
int itemCount = 0;
int currentItem = 0;
unsigned long lastFetch = 0;
unsigned long lastPageSwitch = 0;

bool fetchDashboard() {
  HTTPClient http;
  http.begin(DASHBOARD_URL);
  http.addHeader("X-API-Key", DASHBOARD_API_KEY);
  http.setTimeout(5000);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("GET %s failed, HTTP code %d\n", DASHBOARD_URL, code);
    http.end();
    return false;
  }

  dashboardDoc.clear();
  DeserializationError err = deserializeJson(dashboardDoc, http.getStream());
  http.end();

  if (err) {
    Serial.printf("JSON parse failed: %s\n", err.c_str());
    return false;
  }

  JsonArray containers = dashboardDoc["containers"];
  itemCount = 1 + (containers.isNull() ? 0 : containers.size());

  // Keep cycling from wherever we were -- only clamp if the list shrank
  // (a container got removed) so currentItem can't point past the end.
  currentItem = (itemCount > 0) ? (currentItem % itemCount) : 0;
  return true;
}

void logDashboard() {
  JsonObject host = dashboardDoc["host"];
  JsonArray containers = dashboardDoc["containers"];
  Serial.printf("Fetched OK: host=%s containers=%u\n",
                host.isNull() ? "missing" : "ok",
                containers.isNull() ? 0 : containers.size());
}

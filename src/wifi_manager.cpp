#include "wifi_manager.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "display_hw.h"
#include "secrets.h"

void connectWiFi() {
  showStatus("Connecting WiFi", WIFI_SSID);
  Serial.printf("Connecting to WiFi SSID '%s'", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    esp_task_wdt_reset();  // this loop can legitimately run for a while
                            // during a real outage -- that's not a hang
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nWiFi connect timed out, retrying...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      start = millis();
    }
  }

  Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
  showStatus("WiFi connected", WiFi.localIP().toString());
}

void ensureWiFiConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi dropped, reconnecting...");
    connectWiFi();
  }
}

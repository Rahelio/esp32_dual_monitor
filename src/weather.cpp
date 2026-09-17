#include "weather.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "secrets.h"

WeatherData lastWeather;

bool fetchWeather(WeatherData &out) {
  WiFiClientSecure client;
  client.setInsecure();  // see main.cpp header note on this tradeoff

  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") +
               WEATHER_LAT + "&longitude=" + WEATHER_LON +
               "&current=temperature_2m,weather_code,is_day"
               "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
               "&timezone=auto";

  HTTPClient https;
  if (!https.begin(client, url)) {
    Serial.println("Weather: https.begin() failed");
    return false;
  }
  https.setTimeout(8000);
  // Open-Meteo responds with chunked transfer encoding, which
  // HTTPClient::getStream() doesn't dechunk -- deserializeJson ends up
  // reading chunk-size markers as if they were JSON. Forcing HTTP/1.0
  // avoids chunked encoding entirely (ArduinoJson's own recommended fix).
  https.useHTTP10(true);

  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Weather GET failed, HTTP code %d\n", code);
    https.end();
    return false;
  }

  JsonDocument doc;  // small, separate from dashboardDoc
  DeserializationError err = deserializeJson(doc, https.getStream());
  https.end();
  if (err) {
    Serial.printf("Weather JSON parse failed: %s\n", err.c_str());
    return false;
  }

  JsonObject current = doc["current"];
  if (current.isNull()) {
    Serial.println("Weather: no 'current' block in response");
    return false;
  }

  out.tempC = current["temperature_2m"] | 0.0;
  out.weatherCode = current["weather_code"] | -1;
  out.isDay = (current["is_day"] | 1) == 1;

  // Daily forecast: parallel arrays, index 0 is today.
  JsonObject daily = doc["daily"];
  if (!daily.isNull()) {
    JsonArray tmax = daily["temperature_2m_max"];
    JsonArray tmin = daily["temperature_2m_min"];
    JsonArray precip = daily["precipitation_probability_max"];
    if (tmax.size() > 0) out.tempMaxC = tmax[0];
    if (tmin.size() > 0) out.tempMinC = tmin[0];
    if (precip.size() > 0) out.precipProbMax = precip[0];
  }

  out.valid = true;
  return true;
}

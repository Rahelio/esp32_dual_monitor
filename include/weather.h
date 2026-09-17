#pragma once

struct WeatherData {
  float tempC = 0;
  int weatherCode = -1;
  bool isDay = true;
  float tempMaxC = 0;
  float tempMinC = 0;
  int precipProbMax = -1;  // -1 = not available
  bool valid = false;
};

extern WeatherData lastWeather;

bool fetchWeather(WeatherData &out);

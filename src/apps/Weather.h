#pragma once

#include <ArduinoJson.h>

#include <array>
#include <cstdint>
#include <string>

// Weather app data: the place from the network's IP address (ip-api.com) and
// a 3-day forecast from Open-Meteo. The last forecast is kept on the SD card
// so the app shows it offline.
namespace weather {

enum class Kind : uint8_t { Clear, Partly, Cloudy, Fog, Drizzle, Rain, Snow, Storm };

struct Day {
  int32_t date = 0;  // YYYYMMDD, local to the place
  uint8_t code = 0;  // WMO weather code
  float highC = 0;
  float lowC = 0;
};

constexpr int DAYS = 3;

struct Forecast {
  std::string place;
  int64_t fetchedAt = 0;  // UTC epoch, 0 when unknown
  float tempC = 0;
  uint8_t code = 0;
  uint8_t humidity = 0;
  float windKmh = 0;
  int dayCount = 0;
  std::array<Day, DAYS> days{};
  bool fahrenheit = false;
  bool valid() const { return dayCount > 0; }
};

enum class FetchResult : uint8_t { Ok, NoLocation, NoForecast };

constexpr const char* PATH = "/.crosspoint/weather.json";

Kind kindFor(uint8_t code);

// Parsers for the two API answers; false when a needed field is missing.
bool parseLocation(const JsonDocument& doc, float& lat, float& lon, std::string& place, bool& fahrenheit);
bool parseForecast(const JsonDocument& doc, Forecast& out);

void toJson(const Forecast& forecast, JsonDocument& doc);
bool fromJson(const JsonDocument& doc, Forecast& out);

bool load(Forecast& out);
bool save(const Forecast& forecast);

// Needs Wi-Fi. Blocks for the two requests. Keeps out.fahrenheit when the
// user already chose a unit (keepUnit), else picks it from the country.
FetchResult fetch(Forecast& out, bool keepUnit);

int toDisplay(float celsius, bool fahrenheit);

}  // namespace weather

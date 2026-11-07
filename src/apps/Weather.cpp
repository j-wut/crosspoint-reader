#include "Weather.h"

#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <TrustedTime.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "network/HttpDownloader.h"

// Overridable so the simulator can point at a local test server.
#ifndef WEATHER_GEO_URL
#define WEATHER_GEO_URL "http://ip-api.com/json/?fields=status,city,countryCode,lat,lon"
#endif
#ifndef WEATHER_FORECAST_URL
#define WEATHER_FORECAST_URL "https://api.open-meteo.com/v1/forecast"
#endif

namespace weather {
namespace {

// Both answers are about 1 KB; the cap only guards against a bad server.
constexpr size_t MAX_BODY = 8 * 1024;

bool fetchJson(const std::string& url, JsonDocument& doc) {
  std::string body;
  const bool ok = HttpDownloader::fetchUrl(url, [&body](const uint8_t* data, const size_t len) {
    if (body.size() + len > MAX_BODY) return false;
    body.append(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("WTHR", "Fetch failed (HTTP %d)", HttpDownloader::lastStatus());
    return false;
  }
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    LOG_ERR("WTHR", "Bad JSON (%u bytes)", static_cast<unsigned>(body.size()));
    return false;
  }
  return true;
}

int32_t parseDate(const char* text) {
  int y = 0;
  int m = 0;
  int d = 0;
  if (!text || std::sscanf(text, "%d-%d-%d", &y, &m, &d) != 3) return 0;
  return y * 10000 + m * 100 + d;
}

}  // namespace

Kind kindFor(const uint8_t code) {
  if (code == 0) return Kind::Clear;
  if (code <= 2) return Kind::Partly;
  if (code == 3) return Kind::Cloudy;
  if (code == 45 || code == 48) return Kind::Fog;
  if (code >= 51 && code <= 57) return Kind::Drizzle;
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return Kind::Rain;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return Kind::Snow;
  if (code >= 95) return Kind::Storm;
  return Kind::Cloudy;
}

int toDisplay(const float celsius, const bool fahrenheit) {
  return static_cast<int>(std::lround(fahrenheit ? celsius * 9.0f / 5.0f + 32.0f : celsius));
}

bool parseLocation(const JsonDocument& doc, float& lat, float& lon, std::string& place, bool& fahrenheit) {
  if (std::strcmp(doc["status"] | "", "success") != 0) return false;
  if (!doc["lat"].is<float>() || !doc["lon"].is<float>()) return false;
  lat = doc["lat"];
  lon = doc["lon"];
  place = doc["city"] | "";
  // Countries that use Fahrenheit for weather.
  const char* country = doc["countryCode"] | "";
  fahrenheit = std::strcmp(country, "US") == 0 || std::strcmp(country, "LR") == 0 || std::strcmp(country, "MM") == 0;
  return true;
}

bool parseForecast(const JsonDocument& doc, Forecast& out) {
  const JsonObjectConst current = doc["current"];
  const JsonObjectConst daily = doc["daily"];
  if (current.isNull() || daily.isNull() || !current["temperature_2m"].is<float>()) return false;
  out.tempC = current["temperature_2m"];
  out.code = current["weather_code"] | 0;
  out.humidity = current["relative_humidity_2m"] | 0;
  out.windKmh = current["wind_speed_10m"] | 0.0f;
  const JsonArrayConst dates = daily["time"];
  const JsonArrayConst codes = daily["weather_code"];
  const JsonArrayConst highs = daily["temperature_2m_max"];
  const JsonArrayConst lows = daily["temperature_2m_min"];
  out.dayCount = 0;
  for (size_t i = 0; i < dates.size() && out.dayCount < DAYS; ++i) {
    if (!highs[i].is<float>() || !lows[i].is<float>()) break;
    Day& day = out.days[out.dayCount];
    day.date = parseDate(dates[i] | "");
    if (day.date == 0) break;
    day.code = codes[i] | 0;
    day.highC = highs[i];
    day.lowC = lows[i];
    ++out.dayCount;
  }
  return out.dayCount > 0;
}

void toJson(const Forecast& f, JsonDocument& doc) {
  doc["place"] = f.place;
  doc["fetched"] = f.fetchedAt;
  doc["temp"] = f.tempC;
  doc["code"] = f.code;
  doc["humidity"] = f.humidity;
  doc["wind"] = f.windKmh;
  doc["fahrenheit"] = f.fahrenheit;
  JsonArray days = doc["days"].to<JsonArray>();
  for (int i = 0; i < f.dayCount; ++i) {
    JsonObject day = days.add<JsonObject>();
    day["date"] = f.days[i].date;
    day["code"] = f.days[i].code;
    day["high"] = f.days[i].highC;
    day["low"] = f.days[i].lowC;
  }
}

bool fromJson(const JsonDocument& doc, Forecast& out) {
  out = Forecast{};
  out.fahrenheit = doc["fahrenheit"] | false;
  out.place = doc["place"] | "";
  out.fetchedAt = doc["fetched"] | static_cast<int64_t>(0);
  out.tempC = doc["temp"] | 0.0f;
  out.code = doc["code"] | 0;
  out.humidity = doc["humidity"] | 0;
  out.windKmh = doc["wind"] | 0.0f;
  for (JsonObjectConst day : doc["days"].as<JsonArrayConst>()) {
    if (out.dayCount >= DAYS) break;
    Day& d = out.days[out.dayCount];
    d.date = day["date"] | 0;
    if (d.date == 0) continue;
    d.code = day["code"] | 0;
    d.highC = day["high"] | 0.0f;
    d.lowC = day["low"] | 0.0f;
    ++out.dayCount;
  }
  return true;
}

bool load(Forecast& out) {
  JsonDocument doc;
  out = Forecast{};
  if (!PersistableStoreBase::readDocFromFile(PATH, doc)) return false;
  return fromJson(doc, out);
}

bool save(const Forecast& forecast) {
  JsonDocument doc;
  toJson(forecast, doc);
  return PersistableStoreBase::writeDocToFile(PATH, doc);
}

FetchResult fetch(Forecast& out, const bool keepUnit) {
  float lat = 0;
  float lon = 0;
  std::string place;
  bool fahrenheit = false;
  {
    JsonDocument doc;
    if (!fetchJson(WEATHER_GEO_URL, doc) || !parseLocation(doc, lat, lon, place, fahrenheit)) {
      return FetchResult::NoLocation;
    }
  }
  char url[320];
  std::snprintf(url, sizeof(url),
                "%s?latitude=%.3f&longitude=%.3f&current=temperature_2m,relative_humidity_2m,weather_code,"
                "wind_speed_10m&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=%d",
                WEATHER_FORECAST_URL, lat, lon, DAYS);
  Forecast fresh;
  {
    JsonDocument doc;
    if (!fetchJson(url, doc) || !parseForecast(doc, fresh)) return FetchResult::NoForecast;
  }
  fresh.place = place;
  fresh.fetchedAt = trustedtime::trustedNow();
  fresh.fahrenheit = keepUnit ? out.fahrenheit : fahrenheit;
  out = fresh;
  return FetchResult::Ok;
}

}  // namespace weather

#include "ReadingStats.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <ctime>

namespace readingstats {

int32_t localDate(const int64_t epoch) {
  const auto t = static_cast<time_t>(epoch);
  struct tm local;
  localtime_r(&t, &local);
  return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

bool load(Stats& out) {
  out = Stats{};
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(PATH, doc)) return false;
  out.totalSeconds = doc["total"] | 0u;
  for (JsonArrayConst pair : doc["days"].as<JsonArrayConst>()) {
    if (out.days.size() >= MAX_DAYS) break;
    Day day;
    day.date = pair[0] | 0;
    day.seconds = pair[1] | 0u;
    if (day.date > 0) out.days.push_back(day);
  }
  return true;
}

void addSession(const int64_t startEpoch, const uint32_t seconds) {
  if (startEpoch <= 0 || seconds == 0) return;
  Stats stats;
  load(stats);
  stats.totalSeconds += seconds;
  const int32_t date = localDate(startEpoch);
  if (!stats.days.empty() && stats.days.back().date == date) {
    stats.days.back().seconds += seconds;
  } else if (stats.days.empty() || stats.days.back().date < date) {
    stats.days.push_back({date, seconds});
    if (stats.days.size() > MAX_DAYS) stats.days.erase(stats.days.begin());
  } else {
    // The clock moved backwards; keep the time in the total only.
    LOG_DBG("STATS", "Session day %ld is before the last saved day", static_cast<long>(date));
  }
  JsonDocument doc;
  doc["total"] = stats.totalSeconds;
  JsonArray days = doc["days"].to<JsonArray>();
  for (const auto& day : stats.days) {
    JsonArray pair = days.add<JsonArray>();
    pair.add(day.date);
    pair.add(day.seconds);
  }
  if (!PersistableStoreBase::writeDocToFile(PATH, doc)) LOG_ERR("STATS", "Save failed");
}

}  // namespace readingstats

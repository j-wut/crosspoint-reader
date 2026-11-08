#pragma once

#include <cstdint>
#include <vector>

// Reading time per local day, for the Reading Stats app. Fed from the reader's
// session summary (page dwell time, needs a set clock) when a book closes or
// the device sleeps.
namespace readingstats {

struct Day {
  int32_t date = 0;  // YYYYMMDD
  uint32_t seconds = 0;
};

struct Stats {
  uint32_t totalSeconds = 0;
  std::vector<Day> days;  // oldest first, at most MAX_DAYS
};

constexpr size_t MAX_DAYS = 62;
constexpr const char* PATH = "/.crosspoint/reading_stats.json";

bool load(Stats& out);
// Adds a session that started at startEpoch (UTC seconds) to its local day.
void addSession(int64_t startEpoch, uint32_t seconds);

// YYYYMMDD for a UTC epoch in local time.
int32_t localDate(int64_t epoch);

}  // namespace readingstats

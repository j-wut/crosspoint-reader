#include "AppLists.h"

#include <PersistableStore.h>
#include <TrustedTime.h>

#include <cstdio>
#include <ctime>

namespace applists {
namespace {

std::string clipText(const char* text) {
  std::string out = text ? text : "";
  if (out.size() <= MAX_TEXT) return out;
  // Cut on a UTF-8 boundary.
  size_t cut = MAX_TEXT;
  while (cut > 0 && (static_cast<uint8_t>(out[cut]) & 0xC0) == 0x80) --cut;
  out.resize(cut);
  return out;
}

bool parseDate(const char* text, CountdownItem& item) {
  int y = 0;
  int m = 0;
  int d = 0;
  if (!text || std::sscanf(text, "%d-%d-%d", &y, &m, &d) != 3) return false;
  if (y < 1970 || y > 2200 || m < 1 || m > 12 || d < 1 || d > 31) return false;
  item.year = static_cast<int16_t>(y);
  item.month = static_cast<uint8_t>(m);
  item.day = static_cast<uint8_t>(d);
  return true;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
long daysFromCivil(int y, const unsigned m, const unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
}

}  // namespace

void todosFromJson(const JsonVariantConst doc, std::vector<TodoItem>& out) {
  out.clear();
  for (JsonObjectConst obj : doc["items"].as<JsonArrayConst>()) {
    if (out.size() >= MAX_TODOS) break;
    TodoItem item;
    item.text = clipText(obj["text"] | "");
    if (item.text.empty()) continue;
    item.done = obj["done"] | false;
    item.cloudId = obj["cloudId"] | "";
    out.push_back(std::move(item));
  }
}

void todosToJson(const std::vector<TodoItem>& items, JsonDocument& doc) {
  JsonArray arr = doc["items"].to<JsonArray>();
  for (const auto& item : items) {
    JsonObject obj = arr.add<JsonObject>();
    obj["text"] = item.text;
    obj["done"] = item.done;
    if (!item.cloudId.empty()) obj["cloudId"] = item.cloudId;
  }
}

void countdownsFromJson(const JsonVariantConst doc, std::vector<CountdownItem>& out) {
  out.clear();
  for (JsonObjectConst obj : doc["items"].as<JsonArrayConst>()) {
    if (out.size() >= MAX_COUNTDOWNS) break;
    CountdownItem item;
    item.title = clipText(obj["title"] | "");
    if (item.title.empty() || !parseDate(obj["date"] | "", item)) continue;
    item.cloudId = obj["cloudId"] | "";
    out.push_back(std::move(item));
  }
}

void countdownsToJson(const std::vector<CountdownItem>& items, JsonDocument& doc) {
  JsonArray arr = doc["items"].to<JsonArray>();
  for (const auto& item : items) {
    JsonObject obj = arr.add<JsonObject>();
    obj["title"] = item.title;
    char date[12];
    snprintf(date, sizeof(date), "%04d-%02d-%02d", item.year, item.month, item.day);
    obj["date"] = date;
    if (!item.cloudId.empty()) obj["cloudId"] = item.cloudId;
  }
}

bool loadTodos(std::vector<TodoItem>& out) {
  JsonDocument doc;
  out.clear();
  if (!PersistableStoreBase::readDocFromFile(TODO_PATH, doc)) return false;
  todosFromJson(doc, out);
  return true;
}

bool saveTodos(const std::vector<TodoItem>& items) {
  JsonDocument doc;
  todosToJson(items, doc);
  return PersistableStoreBase::writeDocToFile(TODO_PATH, doc);
}

bool loadCountdowns(std::vector<CountdownItem>& out) {
  JsonDocument doc;
  out.clear();
  if (!PersistableStoreBase::readDocFromFile(COUNTDOWN_PATH, doc)) return false;
  countdownsFromJson(doc, out);
  return true;
}

bool saveCountdowns(const std::vector<CountdownItem>& items) {
  JsonDocument doc;
  countdownsToJson(items, doc);
  return PersistableStoreBase::writeDocToFile(COUNTDOWN_PATH, doc);
}

namespace {
template <typename Item, typename SameText>
void mergeCloud(std::vector<Item>& items, std::vector<Item> cloud, const size_t limit, SameText sameText) {
  for (const auto& item : items) {
    if (cloud.size() >= limit) break;
    if (!item.cloudId.empty()) continue;
    bool duplicate = false;
    for (const auto& c : cloud) duplicate = duplicate || sameText(item, c);
    if (!duplicate) cloud.push_back(item);
  }
  if (cloud.size() > limit) cloud.resize(limit);
  items = std::move(cloud);
}
}  // namespace

void mergeCloudTodos(std::vector<TodoItem>& items, std::vector<TodoItem> cloud) {
  mergeCloud(items, std::move(cloud), MAX_TODOS, [](const TodoItem& a, const TodoItem& b) { return a.text == b.text; });
}

void mergeCloudCountdowns(std::vector<CountdownItem>& items, std::vector<CountdownItem> cloud) {
  mergeCloud(items, std::move(cloud), MAX_COUNTDOWNS,
             [](const CountdownItem& a, const CountdownItem& b) { return a.title == b.title; });
}

bool daysUntil(const CountdownItem& item, int& days) {
  const auto now = static_cast<time_t>(trustedtime::trustedNow());
  if (now == 0) return false;
  struct tm local;
  localtime_r(&now, &local);
  const long today = daysFromCivil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
  days = static_cast<int>(daysFromCivil(item.year, item.month, item.day) - today);
  return true;
}

}  // namespace applists

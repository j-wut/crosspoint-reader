#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

// Data files for the Tiles theme apps. The device apps and the web page both
// load, edit and save these files; neither keeps them open.
namespace applists {

struct TodoItem {
  std::string text;
  bool done = false;
};

struct CountdownItem {
  std::string title;
  // Local calendar date the countdown ends on.
  int16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
};

constexpr size_t MAX_TODOS = 50;
constexpr size_t MAX_COUNTDOWNS = 20;
constexpr size_t MAX_TEXT = 120;

constexpr const char* TODO_PATH = "/.crosspoint/todo.json";
constexpr const char* COUNTDOWN_PATH = "/.crosspoint/countdown.json";

// Parse/serialize helpers shared by the file loaders and the web API. Parsing
// drops entries past the limits and clips text to MAX_TEXT bytes.
void todosFromJson(JsonVariantConst doc, std::vector<TodoItem>& out);
void todosToJson(const std::vector<TodoItem>& items, JsonDocument& doc);
void countdownsFromJson(JsonVariantConst doc, std::vector<CountdownItem>& out);
void countdownsToJson(const std::vector<CountdownItem>& items, JsonDocument& doc);

// A missing file loads as an empty list.
bool loadTodos(std::vector<TodoItem>& out);
bool saveTodos(const std::vector<TodoItem>& items);
bool loadCountdowns(std::vector<CountdownItem>& out);
bool saveCountdowns(const std::vector<CountdownItem>& items);

// Whole days from today (local time) to the item's date; negative once past.
// Returns false while the clock is not set.
bool daysUntil(const CountdownItem& item, int& days);

}  // namespace applists

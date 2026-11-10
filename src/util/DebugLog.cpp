#include "DebugLog.h"

#include <Arduino.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "CrossPointSettings.h"

#if !defined(SIMULATOR)
#include "network/boox/BooxSd.h"
#endif

namespace debuglog {
namespace {

constexpr const char* FEATURE_NAMES[] = {
    "boox-cloud", "boox-cloud-sdfat", "boox-lan",      "boox-lan-sdfat", "boox-bind", "wake-up", "gallery",
    "todo",       "countdown",        "file-transfer", "reader-images",
};
static_assert(sizeof(FEATURE_NAMES) / sizeof(FEATURE_NAMES[0]) == static_cast<size_t>(Feature::Count),
              "one file name per feature");

// LOG_* lines from these origins also go to the feature's file. BOOX lines
// are not listed: the transfer modes keep them with booxdiag::Capture, and
// binding queues its lines from the BLE task.
struct Route {
  const char* origin;
  Feature feature;
};
constexpr Route ROUTES[] = {
    {"SLP", Feature::Gallery},      {"GAL", Feature::Gallery},      {"WALL", Feature::Gallery},
    {"TODO", Feature::Todo},        {"CDN", Feature::Countdown},    {"IMG", Feature::ReaderImages},
    {"JPG", Feature::ReaderImages}, {"PNG", Feature::ReaderImages}, {"GIF", Feature::ReaderImages},
    {"DEC", Feature::ReaderImages}, {"EHP", Feature::ReaderImages}, {"MEM", Feature::ReaderImages},
};

// Lines from other tasks wait here until the main task writes them, because
// SdFat must not be used from two tasks at once. The cap bounds the heap used
// while the card is busy; lines past it are counted, not kept.
constexpr size_t QUEUE_MAX_BYTES = 8 * 1024;
std::mutex queueLock;
std::string queued;  // reader image lines, one per "\n"
size_t droppedLines = 0;

// The current file of each feature. Kept in RTC memory so a wake from deep
// sleep or a restart continues the same file inside its 10 minutes.
struct Current {
  int64_t started;
  uint16_t part;
};
struct State {
  uint32_t magic;
  Current current[static_cast<size_t>(Feature::Count)];
};
constexpr uint32_t STATE_MAGIC = 0x44424C47;  // 'DBLG'
RTC_NOINIT_ATTR State state;

std::mutex lock;
TaskHandle_t mainTask = nullptr;
// Set while this module writes, so log lines from the SD layer do not loop.
volatile bool writing = false;

bool cardHasVfs() {
#if !defined(SIMULATOR)
  return booxsd::stockMounted();
#else
  return false;
#endif
}

bool exists(const char* path) {
#if !defined(SIMULATOR)
  if (cardHasVfs()) return booxsd::fs().exists(path);
#endif
  return Storage.exists(path);
}

size_t fileSize(const char* path) {
  if (!exists(path)) return 0;
#if !defined(SIMULATOR)
  if (cardHasVfs()) {
    fs::File f = booxsd::fs().open(path, FILE_READ);
    return f ? f.size() : 0;
  }
#endif
  HalFile f = Storage.open(path);
  return f ? f.size() : 0;
}

void ensureDir() {
  if (exists(LOG_DIR)) return;
#if !defined(SIMULATOR)
  if (cardHasVfs()) {
    booxsd::fs().mkdir(LOG_DIR);
    return;
  }
#endif
  Storage.mkdir(LOG_DIR);
}

void formatPath(char* out, const size_t len, const Feature feature, const Current& current) {
  const auto started = static_cast<time_t>(current.started);
  struct tm local;
  localtime_r(&started, &local);
  const char* name = FEATURE_NAMES[static_cast<size_t>(feature)];
  if (current.part == 0) {
    snprintf(out, len, "%s/%s-%04d%02d%02d-%02d%02d%02d.txt", LOG_DIR, name, local.tm_year + 1900, local.tm_mon + 1,
             local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
  } else {
    snprintf(out, len, "%s/%s-%04d%02d%02d-%02d%02d%02d-%u.txt", LOG_DIR, name, local.tm_year + 1900, local.tm_mon + 1,
             local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, static_cast<unsigned>(current.part));
  }
}

// Picks the file to append to: a new one after 10 minutes, the next -N part
// once the current one has reached 256 KB.
void currentPath(const Feature feature, char* path, const size_t len) {
  if (state.magic != STATE_MAGIC) {
    memset(&state, 0, sizeof(state));
    state.magic = STATE_MAGIC;
  }
  Current& current = state.current[static_cast<size_t>(feature)];
  const int64_t now = static_cast<int64_t>(time(nullptr));
  // A small step back (clock sync) keeps the file; a large one starts a new one.
  if (current.started == 0 || now < current.started - 60 || now - current.started >= FILE_WINDOW_SECONDS) {
    current.started = now;
    current.part = 0;
  }
  formatPath(path, len, feature, current);
  while (current.part < UINT16_MAX && fileSize(path) >= FILE_MAX_BYTES) {
    current.part++;
    formatPath(path, len, feature, current);
  }
}

void formatStamp(char* out, const size_t len) {
  const time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  snprintf(out, len, "[%04d-%02d-%02d %02d:%02d:%02d +%lums] ", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
           local.tm_hour, local.tm_min, local.tm_sec, static_cast<unsigned long>(millis()));
}

void writeLine(const Feature feature, const char* text, size_t len) {
  while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) --len;
  char stamp[48];
  formatStamp(stamp, sizeof(stamp));
  File file = open(feature);
  if (!file) return;
  file.print(stamp);
  file.write(reinterpret_cast<const uint8_t*>(text), len);
  file.print('\n');
  file.close();
}

// Logging hook: copies routed LOG_* lines to their feature file. Only lines
// from the main task are written here; SdFat must not be used from two tasks
// at once.
void route(const char* origin, const char* text) {
  if (writing || !enabled()) return;
  for (const Route& r : ROUTES) {
    if (strcmp(origin, r.origin) != 0) continue;
    if (xTaskGetCurrentTaskHandle() == mainTask) {
      writeLine(r.feature, text, strlen(text));
    } else if (r.feature == Feature::ReaderImages) {
      size_t len = strlen(text);
      while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) --len;
      std::lock_guard<std::mutex> guard(queueLock);
      if (queued.size() + len + 1 > QUEUE_MAX_BYTES) {
        droppedLines++;
        return;
      }
      queued.append(text, len);
      queued.push_back('\n');
    }
    return;
  }
}

}  // namespace

void begin() {
  mainTask = xTaskGetCurrentTaskHandle();
  setLogRouter(route);
}

bool enabled() { return SETTINGS.debugLogging != 0; }

void flushQueued() {
  std::string lines;
  size_t dropped;
  {
    std::lock_guard<std::mutex> guard(queueLock);
    if (queued.empty() && droppedLines == 0) return;
    lines.swap(queued);
    dropped = droppedLines;
    droppedLines = 0;
  }
  File file = open(Feature::ReaderImages);
  if (!file) return;
  char stamp[48];
  formatStamp(stamp, sizeof(stamp));
  size_t start = 0;
  while (start < lines.size()) {
    size_t end = lines.find('\n', start);
    if (end == std::string::npos) end = lines.size();
    file.print(stamp);
    file.write(reinterpret_cast<const uint8_t*>(lines.data() + start), end - start);
    file.print('\n');
    start = end + 1;
  }
  if (dropped > 0) file.printf("%s%u lines dropped while the card was busy\n", stamp, static_cast<unsigned>(dropped));
}

size_t File::write(const uint8_t c) { return write(&c, 1); }

size_t File::write(const uint8_t* buf, const size_t len) {
  writing = true;
  size_t written = 0;
#if !defined(SIMULATOR)
  if (useVfs_) {
    written = vfs_ ? vfs_.write(buf, len) : 0;
    writing = false;
    return written;
  }
#endif
  written = hal_ ? hal_.write(buf, len) : 0;
  writing = false;
  return written;
}

void File::flush() {
#if !defined(SIMULATOR)
  if (useVfs_) {
    if (vfs_) vfs_.flush();
    return;
  }
#endif
  if (hal_) hal_.flush();
}

void File::close() {
#if !defined(SIMULATOR)
  if (useVfs_) {
    if (vfs_) vfs_.close();
    return;
  }
#endif
  if (hal_) hal_.close();
}

size_t File::size() {
#if !defined(SIMULATOR)
  if (useVfs_) return vfs_ ? vfs_.size() : 0;
#endif
  return hal_ ? hal_.size() : 0;
}

File::operator bool() const {
#if !defined(SIMULATOR)
  if (useVfs_) return static_cast<bool>(vfs_);
#endif
  return static_cast<bool>(hal_);
}

File open(const Feature feature) {
  File file;
  if (!enabled() || feature >= Feature::Count) return file;
  std::lock_guard<std::mutex> guard(lock);
  writing = true;
  ensureDir();
  char path[96];
  currentPath(feature, path, sizeof(path));
#if !defined(SIMULATOR)
  if (cardHasVfs()) {
    file.useVfs_ = true;
    file.vfs_ = booxsd::fs().open(path, FILE_APPEND);
    writing = false;
    return file;
  }
#endif
  file.hal_ = Storage.open(path, O_WRONLY | O_CREAT | O_APPEND);
  writing = false;
  return file;
}

void line(const Feature feature, const char* format, ...) {
  if (!enabled()) return;
  char text[320];
  va_list args;
  va_start(args, format);
  const int len = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (len <= 0) return;
  writeLine(feature, text, std::min(static_cast<size_t>(len), sizeof(text) - 1));
}

void recentDeviceLog(const Feature feature, const char* why) {
  if (!enabled()) return;
  const std::string recent = getLastLogs();
  File file = open(feature);
  if (!file) return;
  file.printf("[%lu s] recent device log (%s), heap %u, max block %u:\n", millis() / 1000, why,
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  file.print(recent.c_str());
  file.println();
}

}  // namespace debuglog

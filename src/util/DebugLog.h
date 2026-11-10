#pragma once

#include <HalStorage.h>
#include <Print.h>

#include <cstddef>
#include <cstdint>

#if !defined(SIMULATOR)
#include <FS.h>
#endif

// Debug log files on the card, written only while System > Debug Logging is on.
//
// Each feature has its own files in /logs, named after the time the file was
// started: /logs/<feature>-YYYYMMDD-HHMMSS.txt. A new file starts every 10
// minutes. A file that reaches 256 KB inside its 10 minutes continues in
// <name>-1.txt, then <name>-2.txt, and so on.
//
// Files are written through the stock BOOX SD stack while it holds the card,
// and through Storage (SdFat) otherwise.
namespace debuglog {

enum class Feature : uint8_t {
  BooxCloud,
  BooxCloudSdFat,
  BooxLan,
  BooxLanSdFat,
  BooxBind,
  WakeUp,
  Gallery,
  Todo,
  Countdown,
  FileTransfer,
  ReaderImages,
  Count
};

constexpr const char* LOG_DIR = "/logs";
constexpr uint32_t FILE_WINDOW_SECONDS = 10 * 60;
constexpr size_t FILE_MAX_BYTES = 256 * 1024;

// Call once from setup() after settings are loaded and the clock is restored.
// Starts sending matching LOG_* lines to the feature files (see DebugLog.cpp).
void begin();

bool enabled();

// Writes routed lines that other tasks (the render task) queued. Call from the
// main task only while no other task uses the card.
void flushQueued();

// A feature's current log file, open for appending. Converts to false when
// debug logging is off or the file cannot be opened; writes then do nothing.
class File final : public Print {
 public:
  File() = default;
  File(File&&) = default;
  File& operator=(File&&) = default;
  ~File() { close(); }
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buf, size_t len) override;
  void flush() override;
  void close();
  size_t size();
  explicit operator bool() const;

 private:
  friend File open(Feature feature);
  HalFile hal_;
#if !defined(SIMULATOR)
  fs::File vfs_;
#endif
  bool useVfs_ = false;
};

File open(Feature feature);

// Append one line, prefixed with the wall-clock time and uptime.
void line(Feature feature, const char* format, ...) __attribute__((format(printf, 2, 3)));

// Append the last lines of the device log ring (also kept across restarts),
// under `why`.
void recentDeviceLog(Feature feature, const char* why);

// The BOOX transfer modes write to separate files per SD stack.
inline Feature booxCloud(const bool sdFat) { return sdFat ? Feature::BooxCloudSdFat : Feature::BooxCloud; }
inline Feature booxLan(const bool sdFat) { return sdFat ? Feature::BooxLanSdFat : Feature::BooxLan; }

}  // namespace debuglog

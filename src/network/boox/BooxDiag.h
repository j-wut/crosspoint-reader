#pragma once

#include <FS.h>
#include <HalStorage.h>
#include <Print.h>

#include <cstddef>

namespace booxdiag {

// While alive, keeps every log line (ours, and ESP-IDF's from Wi-Fi, HTTP, the
// SD card driver and the heap) in a 64 KB PSRAM buffer, so the whole story
// can be written to the card log afterwards. ESP-IDF errors and warnings also
// go to the device log ring, which survives a restart. Only one may be alive.
class Capture {
 public:
  Capture();
  ~Capture();
  Capture(const Capture&) = delete;
  Capture& operator=(const Capture&) = delete;
  // Write the kept lines to `out` and start over.
  static void dumpAndReset(Print& out);
};

// The card log, /tiles-cloud-log.txt. Written through the stock SD stack
// while it holds the card, through SdFat otherwise.
class LogFile final : public Print {
 public:
  LogFile() = default;
  LogFile(LogFile&&) = default;
  LogFile& operator=(LogFile&&) = default;
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buf, size_t len) override;
  void flush() override;
  void close();
  size_t size();
  explicit operator bool() const;

 private:
  friend LogFile openLog();
  HalFile hal_;
  fs::File vfs_;
  bool useVfs_ = false;
};

constexpr const char* LOG_PATH = "/tiles-cloud-log.txt";
// Start a new log instead of growing past this.
constexpr size_t LOG_MAX_BYTES = 256 * 1024;

LogFile openLog();

// One line with the state of everything a transfer depends on: heap, DMA,
// PSRAM, heap integrity, Wi-Fi, battery, stack.
void logSystemState(const char* when);

}  // namespace booxdiag

#pragma once

#include <Print.h>

#include <cstddef>

namespace booxdiag {

// While alive, keeps every log line (ours, and ESP-IDF's from Wi-Fi, HTTP, the
// SD card driver and the heap) in a 64 KB PSRAM buffer, so the whole story
// can be written to the debug log afterwards. ESP-IDF errors and warnings also
// go to the device log ring, which survives a restart. Only one may be alive.
// Does nothing while Debug Logging is off.
class Capture {
 public:
  Capture();
  ~Capture();
  Capture(const Capture&) = delete;
  Capture& operator=(const Capture&) = delete;
  // Write the kept lines to `out` and start over.
  static void dumpAndReset(Print& out);

 private:
  bool active_ = false;
};

// One line with the state of everything a transfer depends on: heap, DMA,
// PSRAM, heap integrity, Wi-Fi, battery, stack.
void logSystemState(const char* when);

// One line with the power side of a transfer: Wi-Fi power save and TX power
// limit, CPU clock, the SD/panel rail enable, frontlight, USB and battery.
void logPowerState(const char* when);

// Ring of the last SD write times, printed after a failed write to show
// whether the card slowed down first.
void noteWrite(size_t offset, size_t len, uint32_t ms, bool ok);
void resetWrites();
void logWrites(const char* when);

}  // namespace booxdiag

#pragma once

#include <FS.h>
#include <HalStorage.h>

// The card for the BOOX transfer modes, through one of two stacks:
// - Stock: the stock Picco firmware's Arduino SD_MMC (4-bit, 40 MHz, 15 open
//   files, mounted at /sdcard) over ESP-IDF's FAT driver with exFAT on. SdFat
//   (Storage) is shut down; leaving file transfer restarts the device, which
//   mounts SdFat again.
// - SdFat: the firmware's own SdFat (Storage), left as it is.
// Paths are relative to the card root ("/books/x").
namespace booxsd {

enum class Backend : uint8_t { Stock, SdFat };
const char* backendName(Backend backend);

// A file open for writing on the selected stack.
class File {
 public:
  size_t write(const uint8_t* data, size_t len);
  void flush();
  void close();
  explicit operator bool() const;

 private:
  friend File openWrite(const char* path);
  fs::File vfs_;
  HalFile hal_;
  bool sdfat_ = false;
};

// Select the stack and mount the card. Logs every step.
bool mount(Backend backend);
void unmount();
// Unmount and mount again on the same stack, so the log can still be written
// after the card stopped answering. Logs the result.
bool remount(const char* why);
bool mounted();
Backend backend();
// True while the stock stack holds the card (SdFat is shut down).
bool stockMounted();
// The stock stack's file system (valid while stockMounted()).
fs::FS& fs();

// File operations on the selected stack.
File openWrite(const char* path);  // create or truncate
bool exists(const char* path);
bool mkdir(const char* path);
bool remove(const char* path);
bool rename(const char* from, const char* to);
size_t fileSize(const char* path);
// Card size and free space in bytes (total is 0 on SdFat). False when not mounted.
bool spaceInfo(uint64_t& total, uint64_t& freeBytes);
// One log line with the card, the file system type, and free space.
void logState(const char* when);

}  // namespace booxsd

#pragma once

#include <FS.h>

// The SD card stack of the stock Picco firmware, for the BOOX transfer modes:
// Arduino SD_MMC (4-bit, 40 MHz, 15 open files, mounted at /sdcard) over
// ESP-IDF's FAT driver with exFAT on. While mounted, SdFat (Storage) is shut
// down; leaving file transfer restarts the device, which mounts SdFat again.
namespace booxsd {

// Shut down SdFat and mount the card. Logs every step.
bool mount();
void unmount();
bool mounted();
// The mounted file system; paths are relative to the card root ("/books/x").
fs::FS& fs();
// Card size and free space in bytes. False when not mounted.
bool spaceInfo(uint64_t& total, uint64_t& freeBytes);
// One log line with the card, the file system type, and free space.
void logState(const char* when);

}  // namespace booxsd

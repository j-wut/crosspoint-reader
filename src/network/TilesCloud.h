#pragma once

#include <string>

// BOOX Tiles cloud support for the Picco. The stock firmware keeps its binding
// to a BOOX account in NVS, which survives flashing this firmware.
namespace tilescloud {

// mDNS hostname: "Picco-<efuse MAC hex>" on the Picco, as the stock firmware
// and its binding QR use; `fallback` on other boards.
std::string mdnsHostname(const char* fallback);

// Add the stock firmware's _nanoreader._tcp service and TXT records (Picco only).
void addMdnsService();

// Diagnostic: write the saved binding's layout (never secret values) and the
// answers of the BOOX push list and a few read-only endpoints to
// /tiles-cloud-log.txt, and try to download pushed items. Needs Wi-Fi with
// internet access. Picco only; does nothing elsewhere.
void probe();

// Check the push list again with the token found by probe(), and download
// new items to the SD card root. Picco only.
void poll();

// Append one printf-style line to the same log (Picco only).
void logLine(const char* format, ...) __attribute__((format(printf, 1, 2)));

}  // namespace tilescloud

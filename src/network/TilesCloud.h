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
// answer of the BOOX push list to /tiles-cloud-log.txt. Needs Wi-Fi with
// internet access. Picco only; does nothing elsewhere.
void probe();

}  // namespace tilescloud

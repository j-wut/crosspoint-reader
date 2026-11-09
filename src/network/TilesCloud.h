#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "apps/AppLists.h"

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

// Result of the last probe() or poll(), for the BOOX Cloud screen.
struct Status {
  bool bound = false;  // a saved binding with a token was found
  int lastHttp = 0;    // HTTP status of the last push list check (0: none)
  int waiting = 0;     // items in the last push list
  int received = 0;    // items downloaded this session
  int failed = 0;      // downloads that failed this session
  // Steps of the last push item download, shown on screen in case the SD
  // card log cannot be written after it.
  static constexpr int MAX_ATTEMPTS = 3;
  char attempts[MAX_ATTEMPTS][64] = {};
  int attemptCount = 0;
};
const Status& status();

// Fetch the BOOX Tiles todo or countdown list and merge it into `items`, as
// the stock firmware does when its Todo or Countdown app opens: join the last
// Wi-Fi network if needed, GET /todo or /countdownDay with the saved token,
// then turn Wi-Fi off again if this call turned it on. Blocks for the
// requests. Each step goes to /tiles-cloud-log.txt. Picco only.
enum class SyncResult : uint8_t { Skipped, NoBinding, NoWifi, Failed, Ok };
SyncResult syncTodos(std::vector<applists::TodoItem>& items);
SyncResult syncCountdowns(std::vector<applists::CountdownItem>& items);

// After the app has bound this device over BLE: ask /users/deviceToken for
// a token (the app links the device's MAC to the account), read /users/me
// with it and save both to NVS user_cred like the stock firmware (userInfo,
// token, deviceId, secretHex, tokenLastAt). False while the server has no
// token for this device yet. Needs Wi-Fi. Picco only.
bool finishBinding();

// Append the device's recent log lines to the same log, under `why` (Picco only).
void logRecentDeviceLog(const char* why);

// Append one printf-style line to the same log (Picco only).
void logLine(const char* format, ...) __attribute__((format(printf, 1, 2)));

}  // namespace tilescloud

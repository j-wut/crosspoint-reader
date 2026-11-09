#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// BLE transport for BOOX Tiles binding, kept apart from CrossPoint's other
// Bluetooth code. It follows Espressif's protocomm_nimble.c, which the stock
// Picco firmware uses: the same GATT table (GATT service, then the
// provisioning service; no GAP service), advertising, security settings and
// connection handling. Only the NimBLE host itself is shared.
namespace tilesble {

struct Endpoint {
  uint16_t uuid16;  // replaces bytes 12-13 of the service UUID
  const char* name;
};

struct Callbacks {
  // A write to endpoint `index`; returns the reply the app reads next.
  std::string (*onWrite)(size_t index, const std::string& request);
  void (*onConnect)();
  void (*onDisconnect)(int reason);
  void (*log)(const char* line);
};

// serviceUuid is the 16 raw bytes, least significant first (NimBLE order).
bool start(const std::string& deviceName, const uint8_t serviceUuid[16], const Endpoint* endpoints, size_t count,
           const Callbacks& callbacks);
void stop();

}  // namespace tilesble

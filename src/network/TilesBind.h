#pragma once

#include <cstdint>
#include <string>

// Binding the Picco to the BOOX Tiles app, the way the stock firmware does:
// Espressif network provisioning ("netprov-v1.2", security 1 with a proof of
// possession from the QR code) over a BLE GATT service. The app sends Wi-Fi
// credentials, links the device's MAC to the account in the cloud, and the
// device then fetches its token from /users/deviceToken. Picco only.
namespace tilesbind {

enum class Stage : uint8_t {
  Off,
  Advertising,     // waiting for the app
  Connected,       // app connected over BLE
  SessionReady,    // secure session set up, waiting for Wi-Fi details
  WifiConnecting,  // app sent Wi-Fi details
  WifiConnected,
  WifiFailed,
  Failed,  // BLE could not start
};

// Start advertising and fill `qrUrl` with the stock binding QR link.
bool start(std::string& qrUrl);
// Stop BLE and free it. Safe to call when not started.
void stop();
Stage stage();
// SSID the app sent (empty until then).
std::string ssid();
// Password the app sent, for saving the network once it connects.
std::string passphrase();
// Tell a connected phone the GATT table changed (stock does this when its
// firmware changes). Call from the activity loop.
void serviceChanged();
// Write queued log lines (from the BLE task) to the cloud log. Call from the
// activity loop.
void flushLog();

}  // namespace tilesbind

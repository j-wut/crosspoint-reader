#pragma once

#include <string>

#include "activities/Activity.h"
#include "network/TilesBind.h"

// Shows the stock binding QR code and runs BLE provisioning until the BOOX
// Tiles app has sent Wi-Fi details and the account link arrives (Picco only).
class TilesBindActivity final : public Activity {
 public:
  explicit TilesBindActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TilesBind", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return !bound && !timedOut; }

 private:
  enum class Phase : uint8_t { Starting, Waiting, Linking, Bound, TimedOut, Failed };
  Phase phase() const;

  std::string qrUrl;
  bool started = false;
  bool wifiSaved = false;
  bool bound = false;
  bool timedOut = false;
  unsigned long linkStartedAt = 0;
  unsigned long lastLinkTry = 0;
  unsigned long boundAt = 0;
  tilesbind::Stage shownStage = tilesbind::Stage::Off;
};

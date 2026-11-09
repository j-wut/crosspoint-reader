#pragma once

#include "activities/Activity.h"

// Power Off confirmation, laid out like the stock BOOX Picco popup: a small
// card at the top-right corner over the current screen. Tapping the card or
// pressing Power powers off; tapping elsewhere or another button closes it.
class PowerOffPopupActivity final : public Activity {
 public:
  PowerOffPopupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PowerOffPopup", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};

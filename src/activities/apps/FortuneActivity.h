#pragma once

#include "activities/Activity.h"
#include "components/UiAppHost.h"

// Draws a short saying at random; tap the card or press Confirm for another.
class FortuneActivity final : public Activity, private UiAppHost {
 public:
  FortuneActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Fortune", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void drawAgain();

  int current = 0;
  bool pending = false;
};

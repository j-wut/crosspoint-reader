#pragma once

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Tiles theme Apps screen: a paged 3x3 grid of app tiles.
class AppsActivity final : public Activity, private UiAppHost {
 public:
  enum class App : uint8_t { ReadingStats, Todo, Pomodoro, Countdown, Fortune, Files };

  AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Apps", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int COLUMNS = 3;
  static constexpr int ROWS = 3;
  static constexpr int PER_PAGE = COLUMNS * ROWS;

  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void open(int index);
  void select(int index);

  ButtonNavigator buttonNavigator;
  int selected = 0;
  int pending = -1;
};

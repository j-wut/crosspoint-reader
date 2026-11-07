#pragma once

#include "activities/Activity.h"
#include "apps/ReadingStats.h"
#include "components/UiAppHost.h"

// Reading time totals and a seven-day chart from readingstats.
class ReadingStatsActivity final : public Activity, private UiAppHost {
 public:
  ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static void screenFn(UiScreen& screen, void* user);
  void draw(UiScreen& screen);

  readingstats::Stats stats;
  int32_t today = 0;  // YYYYMMDD, 0 while the clock is unset
};

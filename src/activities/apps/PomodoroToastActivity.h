#pragma once

#include <string>

#include "activities/Activity.h"

// Shown over the current screen when a Pomodoro timer ends while the
// Pomodoro screen is not open. Any tap or button closes it.
class PomodoroToastActivity final : public Activity {
 public:
  PomodoroToastActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* message)
      : Activity("PomodoroToast", renderer, mappedInput), message(message) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const char* message;
};

#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Focus timer: a focus session, then a break. While running, the screen shows
// minutes and seconds left and refreshes each second, like the stock Picco
// app. The device stays awake; when time is up the screen flashes and the
// front light blinks.
class PomodoroActivity final : public Activity, private UiAppHost {
 public:
  PomodoroActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Pomodoro", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return running || blinksLeft > 0; }

 private:
  enum class Phase : uint8_t { Focus, Break };
  enum Button : int16_t { START, PAUSE, RESUME, GIVE_UP, SKIP_BREAK, LONGER, SHORTER };
  static constexpr int MAX_BUTTONS = 3;

  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  int buttons(Button out[MAX_BUTTONS]) const;
  void press(Button button);
  void startPhase(Phase next);
  void finishPhase();
  uint32_t phaseMs() const;
  uint32_t remainingMs() const;
  void loadState();
  void saveState() const;
  void tickBlink();

  Phase phase = Phase::Focus;
  bool running = false;
  bool paused = false;
  bool timeUp = false;          // "Time's up" shown until the next button
  uint32_t startedAt = 0;       // millis() when the current run started
  uint32_t elapsedBefore = 0;   // ms run before the last pause
  uint32_t lastShownSecond = 0;
  uint8_t focusMinutes = 25;
  uint8_t breakMinutes = 5;
  uint16_t completed = 0;       // focus sessions finished today
  int32_t completedDay = -1;    // local day number of `completed`, -1 = clock unset
  int selected = 0;
  int pending = -1;
  int blinksLeft = 0;
  uint32_t lastBlinkAt = 0;
  bool lightWasOn = false;
  bool flashPending = false;
  ButtonNavigator buttonNavigator;
};

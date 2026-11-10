#include "PomodoroToastActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"

void PomodoroToastActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void PomodoroToastActivity::loop() {
  int x = 0;
  int y = 0;
  bool close = mappedInput.wasScreenTapped(x, y);
  for (uint8_t b = 0; !close && b < static_cast<uint8_t>(MappedInputManager::Button::Power); ++b) {
    close = mappedInput.wasReleased(static_cast<MappedInputManager::Button>(b));
  }
  if (close) finish();
}

// Drawn over the retained frame of the screen underneath.
void PomodoroToastActivity::render(RenderLock&&) { GUI.drawPopup(renderer, message); }

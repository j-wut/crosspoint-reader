#include "PomodoroActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <I18n.h>
#include <PersistableStore.h>
#include <TrustedTime.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "MappedInputManager.h"
#include "components/BigDigits.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId PRESS = 1;
constexpr const char* STATE_PATH = "/.crosspoint/pomodoro.json";
constexpr uint32_t MINUTE_MS = 60UL * 1000UL;
constexpr int BLINKS = 6;  // light toggles when time is up
constexpr uint32_t BLINK_MS = 400;
constexpr int16_t DIGIT_HEIGHT = 120;
constexpr int16_t BUTTON_HEIGHT = 56;
constexpr uint8_t MIN_FOCUS = 5;
constexpr uint8_t MAX_FOCUS = 90;
// Timer state saved in STATE_PATH.
constexpr int MODE_IDLE = 0;
constexpr int MODE_RUNNING = 1;
constexpr int MODE_PAUSED = 2;
constexpr int MODE_TIME_UP = 3;

// Local day number, or -1 while the clock is unset.
int32_t today() {
  const auto now = static_cast<time_t>(trustedtime::trustedNow());
  if (now == 0) return -1;
  struct tm local;
  localtime_r(&now, &local);
  return (local.tm_year + 1900) * 366 + local.tm_yday;
}
}  // namespace

void PomodoroActivity::onEnter() {
  Activity::onEnter();
  loadState();
  resetUi();
  app.on(PRESS, &PomodoroActivity::onAction, this);
  app.setScreen(&PomodoroActivity::screenFn, this);
  requestUpdate();
}

void PomodoroActivity::onExit() {
  if (blinksLeft > 0) Frontlight.setOn(lightWasOn);
  blinksLeft = 0;
  Activity::onExit();
}

void PomodoroActivity::loadState() {
  const bool firstLoad = !stateLoaded;
  stateLoaded = true;
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(STATE_PATH, doc)) return;
  focusMinutes = std::clamp<int>(doc["focus"] | 25, MIN_FOCUS, MAX_FOCUS);
  breakMinutes = std::clamp<int>(doc["break"] | 5, 1, 30);
  completed = doc["completed"] | 0;
  completedDay = doc["day"] | -1;
  const int32_t day = today();
  if (day >= 0 && completedDay != day) completed = 0;
  if (!firstLoad) return;

  // After a reboot or wake, restore the timer saved before sleep.
  phase = (doc["phase"] | 0) == 1 ? Phase::Break : Phase::Focus;
  const int mode = doc["mode"] | MODE_IDLE;
  startedAt = millis();
  if (mode == MODE_PAUSED) {
    paused = true;
    elapsedBefore = std::min<uint32_t>(doc["elapsed"] | 0, phaseMs());
  } else if (mode == MODE_TIME_UP) {
    timeUp = true;
    elapsedBefore = phaseMs();
  } else if (mode == MODE_RUNNING) {
    // The end time is wall-clock seconds; without a trusted clock it is lost.
    const int64_t endAt = doc["endAt"] | static_cast<int64_t>(0);
    const int64_t now = trustedtime::trustedNow();
    if (endAt > 0 && now > 0) {
      const int64_t leftMs = std::max<int64_t>(0, (endAt - now) * 1000);
      running = true;
      elapsedBefore = leftMs >= phaseMs() ? 0 : phaseMs() - static_cast<uint32_t>(leftMs);
    } else {
      phase = Phase::Focus;
    }
  }
}

void PomodoroActivity::saveState() {
  JsonDocument doc;
  doc["focus"] = focusMinutes;
  doc["break"] = breakMinutes;
  doc["completed"] = completed;
  doc["day"] = completedDay;
  doc["phase"] = phase == Phase::Break ? 1 : 0;
  if (running) {
    doc["mode"] = MODE_RUNNING;
    const int64_t now = trustedtime::trustedNow();
    doc["endAt"] = now > 0 ? now + (remainingMs() + 999) / 1000 : 0;
  } else if (paused) {
    doc["mode"] = MODE_PAUSED;
    doc["elapsed"] = elapsedBefore;
  } else {
    doc["mode"] = timeUp ? MODE_TIME_UP : MODE_IDLE;
  }
  PersistableStoreBase::writeDocToFile(STATE_PATH, doc);
}

uint32_t PomodoroActivity::phaseMs() {
  return static_cast<uint32_t>(phase == Phase::Focus ? focusMinutes : breakMinutes) * MINUTE_MS;
}

uint32_t PomodoroActivity::remainingMs() {
  const uint32_t elapsed = elapsedBefore + (running ? millis() - startedAt : 0);
  return elapsed >= phaseMs() ? 0 : phaseMs() - elapsed;
}

void PomodoroActivity::startPhase(const Phase next) {
  phase = next;
  running = true;
  paused = false;
  timeUp = false;
  elapsedBefore = 0;
  startedAt = millis();
  lastShownSecond = (remainingMs() + 999) / 1000;
  selected = 0;
}

void PomodoroActivity::endTimer() {
  running = false;
  paused = false;
  timeUp = true;
  elapsedBefore = phaseMs();
  if (phase == Phase::Focus) {
    const int32_t day = today();
    if (day >= 0 && day != completedDay) completed = 0;
    completedDay = day;
    ++completed;
  }
  saveState();
}

const char* PomodoroActivity::takeBackgroundTimeUp() {
  if (!stateLoaded) loadState();
  if (!running || remainingMs() > 0) return nullptr;
  endTimer();
  return phase == Phase::Focus ? tr(STR_POMODORO_FOCUS_DONE) : tr(STR_POMODORO_BREAK_DONE);
}

void PomodoroActivity::finishPhase() {
  endTimer();
  // Blink the front light and flash the panel so the end is noticed.
  if (Frontlight.present()) {
    lightWasOn = Frontlight.isOn();
    blinksLeft = BLINKS;
    lastBlinkAt = 0;
  }
  flashPending = true;
  selected = 0;
}

void PomodoroActivity::tickBlink() {
  if (blinksLeft <= 0) return;
  const uint32_t now = millis();
  if (lastBlinkAt != 0 && now - lastBlinkAt < BLINK_MS) return;
  lastBlinkAt = now;
  --blinksLeft;
  // Even toggles end on the starting state.
  Frontlight.setOn(blinksLeft % 2 == 0 ? lightWasOn : !lightWasOn);
}

int PomodoroActivity::buttons(Button out[MAX_BUTTONS]) const {
  int n = 0;
  if (running) {
    out[n++] = PAUSE;
    out[n++] = phase == Phase::Focus ? GIVE_UP : SKIP_BREAK;
  } else if (paused) {
    out[n++] = RESUME;
    out[n++] = phase == Phase::Focus ? GIVE_UP : SKIP_BREAK;
  } else {
    out[n++] = START;
    if (!timeUp || phase == Phase::Break) {
      out[n++] = SHORTER;
      out[n++] = LONGER;
    } else {
      out[n++] = SKIP_BREAK;
    }
  }
  return n;
}

void PomodoroActivity::press(const Button button) {
  switch (button) {
    case START:
      // After a focus session the next start is its break.
      startPhase(timeUp && phase == Phase::Focus ? Phase::Break : Phase::Focus);
      saveState();
      break;
    case PAUSE:
      elapsedBefore += millis() - startedAt;
      running = false;
      paused = true;
      saveState();
      break;
    case RESUME:
      startedAt = millis();
      running = true;
      paused = false;
      saveState();
      break;
    case GIVE_UP:
    case SKIP_BREAK:
      running = false;
      paused = false;
      timeUp = false;
      phase = Phase::Focus;
      elapsedBefore = 0;
      selected = 0;
      saveState();
      break;
    case LONGER:
    case SHORTER: {
      const int step = button == LONGER ? 5 : -5;
      focusMinutes = static_cast<uint8_t>(std::clamp<int>(focusMinutes + step, MIN_FOCUS, MAX_FOCUS));
      phase = Phase::Focus;
      timeUp = false;
      elapsedBefore = 0;
      saveState();
      break;
    }
  }
  requestUpdate();
}

void PomodoroActivity::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<PomodoroActivity*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

void PomodoroActivity::screenFn(UiScreen& screen, void* user) { static_cast<PomodoroActivity*>(user)->draw(screen); }

void PomodoroActivity::loop() {
  tickBlink();
  if (running) {
    const uint32_t left = remainingMs();
    if (left == 0) {
      finishPhase();
      requestUpdate();
      return;
    }
    const uint32_t second = (left + 999) / 1000;
    if (second != lastShownSecond) {
      lastShownSecond = second;
      requestUpdate();
    }
  }

  pending = -1;
  const auto touch = routeTouch(mappedInput);
  if (touch.snap.touchReleased && pending >= 0) {
    press(static_cast<Button>(pending));
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  Button list[MAX_BUTTONS];
  const int count = buttons(list);
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    press(list[std::min(selected, count - 1)]);
    return;
  }
  buttonNavigator.onNext([this, count] {
    selected = ButtonNavigator::nextIndex(selected, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selected = ButtonNavigator::previousIndex(selected, count);
    requestUpdate();
  });
}

void PomodoroActivity::draw(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width) + metrics.contentSidePadding),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
      static_cast<int16_t>(safe.x + metrics.contentSidePadding)});
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const fui::Rect body = screen.body();
  const int16_t titleHeight = screen.target().lineHeight(theme.titleText.font);
  const int16_t smallHeight = screen.target().lineHeight(theme.smallText.font);

  auto y = static_cast<int16_t>(body.y + body.height / 10);
  auto title = theme.titleText;
  title.bold = true;
  title.align = fui::TextAlign::Center;
  const char* phaseLabel = phase == Phase::Focus ? tr(STR_POMODORO_FOCUS) : tr(STR_POMODORO_BREAK);
  screen.target().text(fui::Rect{body.x, y, body.width, titleHeight}, timeUp ? tr(STR_POMODORO_TIME_UP) : phaseLabel,
                       title);
  y = static_cast<int16_t>(y + titleHeight + theme.spaceLg * 2);

  const uint32_t left = timeUp && phase == Phase::Focus ? static_cast<uint32_t>(breakMinutes) * MINUTE_MS
                        : timeUp                         ? phaseMs()
                                                         : remainingMs();
  const uint32_t secs = (left + 999) / 1000;
  char digits[8];
  snprintf(digits, sizeof(digits), "%02u:%02u", static_cast<unsigned>(secs / 60), static_cast<unsigned>(secs % 60));
  const int16_t digitsWidth = bigdigits::width(digits, DIGIT_HEIGHT);
  bigdigits::draw(screen.target(), static_cast<int16_t>(body.x + (body.width - digitsWidth) / 2), y, digits,
                  DIGIT_HEIGHT);
  y = static_cast<int16_t>(y + DIGIT_HEIGHT + theme.spaceLg * 2);

  fui::ProgressBarProps bar;
  bar.value = static_cast<int32_t>((phaseMs() - std::min(phaseMs(), left)) / 1000);
  bar.max = static_cast<int32_t>(phaseMs() / 1000);
  if (timeUp) bar.value = 0;
  bar.border = ink;
  bar.borderWidth = 2;
  bar.radius = 6;
  fui::progressBar(screen.frame(), fui::Rect{body.x, y, body.width, 12}, bar);
  y = static_cast<int16_t>(y + 12 + theme.spaceLg);

  auto info = theme.smallText;
  info.align = fui::TextAlign::Center;
  char line[64];
  snprintf(line, sizeof(line), tr(STR_POMODORO_COMPLETED), completed);
  screen.target().text(fui::Rect{body.x, y, body.width, smallHeight}, line, info);
  y = static_cast<int16_t>(y + smallHeight + theme.spaceSm);
  if (timeUp && phase == Phase::Focus) {
    snprintf(line, sizeof(line), tr(STR_POMODORO_BREAK_NEXT), breakMinutes);
    screen.target().text(fui::Rect{body.x, y, body.width, smallHeight}, line, info);
  }

  // Buttons along the bottom.
  Button list[MAX_BUTTONS];
  const int count = buttons(list);
  const int16_t gap = theme.spaceMd;
  const auto bw = static_cast<int16_t>((body.width - gap * (count - 1)) / count);
  const auto by = static_cast<int16_t>(body.bottom() - BUTTON_HEIGHT - theme.spaceLg);
  for (int i = 0; i < count; ++i) {
    const fui::Rect r{static_cast<int16_t>(body.x + i * (bw + gap)), by, bw, BUTTON_HEIGHT};
    const bool isSelected = i == selected && !BoardConfig::hasTouch();
    if (isSelected) {
      screen.target().fill(r, ink, theme.controlRadius);
    } else {
      screen.target().stroke(r, ink, 2, theme.controlRadius);
    }
    const char* label = "";
    switch (list[i]) {
      case START:
        label = timeUp && phase == Phase::Focus ? tr(STR_POMODORO_START_BREAK) : tr(STR_POMODORO_START);
        break;
      case PAUSE:
        label = tr(STR_POMODORO_PAUSE);
        break;
      case RESUME:
        label = tr(STR_RESUME);
        break;
      case GIVE_UP:
        label = tr(STR_POMODORO_GIVE_UP);
        break;
      case SKIP_BREAK:
        label = tr(STR_POMODORO_SKIP_BREAK);
        break;
      case LONGER:
        label = "+5";
        break;
      case SHORTER:
        label = "-5";
        break;
    }
    auto text = theme.bodyText;
    text.align = fui::TextAlign::Center;
    text.inverted = isSelected;
    const int16_t lh = screen.target().lineHeight(text.font);
    screen.target().text(fui::Rect{r.x, static_cast<int16_t>(r.y + (r.height - lh) / 2), r.width, lh}, label, text);
    screen.frame().hit(r, PRESS, static_cast<int16_t>(list[i]), fui::InputTouch);
  }
}

void PomodoroActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_APP_POMODORO));
  renderUi();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(flashPending ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  flashPending = false;
}

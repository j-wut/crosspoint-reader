#include "AppsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <iterator>

#include "CountdownActivity.h"
#include "FortuneActivity.h"
#include "MappedInputManager.h"
#include "PomodoroActivity.h"
#include "ReadingStatsActivity.h"
#include "TodoActivity.h"
#include "WeatherActivity.h"
#include "activities/ActivityManager.h"
#include "components/TilesGrid.h"
#include "components/UITheme.h"
#include "components/icons/appIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId OPEN = 1;

struct AppEntry {
  AppsActivity::App app;
  StrId label;
  const freeink::Icon* icon;
};

const AppEntry APPS[] = {
    {AppsActivity::App::ReadingStats, StrId::STR_APP_READING_STATS, &icon_app_stats_48},
    {AppsActivity::App::Todo, StrId::STR_APP_TODO, &icon_app_todo_48},
    {AppsActivity::App::Pomodoro, StrId::STR_APP_POMODORO, &icon_app_pomodoro_48},
    {AppsActivity::App::Countdown, StrId::STR_APP_COUNTDOWN, &icon_app_countdown_48},
    {AppsActivity::App::Weather, StrId::STR_APP_WEATHER, &icon_app_weather_48},
    {AppsActivity::App::Fortune, StrId::STR_APP_FORTUNE, &icon_app_fortune_48},
    {AppsActivity::App::Files, StrId::STR_BROWSE_FILES, &icon_app_files_48},
};
constexpr int APP_COUNT = static_cast<int>(std::size(APPS));
}  // namespace

void AppsActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(OPEN, &AppsActivity::onAction, this);
  app.setScreen(&AppsActivity::screenFn, this);
  requestUpdate();
}

void AppsActivity::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<AppsActivity*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

void AppsActivity::screenFn(UiScreen& screen, void* user) { static_cast<AppsActivity*>(user)->draw(screen); }

void AppsActivity::select(const int index) {
  selected = index;
  requestUpdate();
}

void AppsActivity::open(const int index) {
  if (index < 0 || index >= APP_COUNT) return;
  selected = index;
  std::unique_ptr<Activity> next;
  switch (APPS[index].app) {
    case App::ReadingStats:
      next = makeUniqueNoThrow<ReadingStatsActivity>(renderer, mappedInput);
      break;
    case App::Todo:
      next = makeUniqueNoThrow<TodoActivity>(renderer, mappedInput);
      break;
    case App::Pomodoro:
      next = makeUniqueNoThrow<PomodoroActivity>(renderer, mappedInput);
      break;
    case App::Countdown:
      next = makeUniqueNoThrow<CountdownActivity>(renderer, mappedInput);
      break;
    case App::Weather:
      next = makeUniqueNoThrow<WeatherActivity>(renderer, mappedInput);
      break;
    case App::Fortune:
      next = makeUniqueNoThrow<FortuneActivity>(renderer, mappedInput);
      break;
    case App::Files:
      activityManager.goToFileBrowser();
      return;
  }
  if (!next) {
    LOG_ERR("APPS", "OOM: app %d", index);
    return;
  }
  activityManager.pushActivity(std::move(next));
}

void AppsActivity::loop() {
  pending = -1;
  const auto touch = routeTouch(mappedInput);
  if (touch.snap.touchReleased && pending >= 0) {
    open(pending);
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  const int page = selected / tilesgrid::PER_PAGE;
  const int pages = (APP_COUNT + tilesgrid::PER_PAGE - 1) / tilesgrid::PER_PAGE;
  if ((swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) && page + 1 < pages) {
    select((page + 1) * tilesgrid::PER_PAGE);
    return;
  }
  if ((swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) && page > 0) {
    select((page - 1) * tilesgrid::PER_PAGE);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome(HomeMenuItem::APPS);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    open(selected);
    return;
  }
  buttonNavigator.onNext([this] { select(ButtonNavigator::nextIndex(selected, APP_COUNT)); });
  buttonNavigator.onPrevious([this] { select(ButtonNavigator::previousIndex(selected, APP_COUNT)); });
}

void AppsActivity::draw(UiScreen& screen) {
  tilesgrid::setContentMargin(screen, renderer);
  tilesgrid::Tile tiles[APP_COUNT];
  for (int i = 0; i < APP_COUNT; ++i) {
    tiles[i] = {I18n::getInstance().get(APPS[i].label), APPS[i].icon};
  }
  tilesgrid::draw(screen, tiles, APP_COUNT, selected, OPEN);
}

void AppsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_APPS));
  renderUi();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

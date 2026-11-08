#include "AppsActivity.h"

#include <BoardConfig.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <iterator>

#include "CountdownActivity.h"
#include "FortuneActivity.h"
#include "GalleryActivity.h"
#include "MappedInputManager.h"
#include "PomodoroActivity.h"
#include "ReadingStatsActivity.h"
#include "TodoActivity.h"
#include "WeatherActivity.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/icons/appIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId OPEN = 1;
constexpr int16_t TILE_GAP = 12;
constexpr int16_t TILE_ICON = 48;

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
    {AppsActivity::App::Gallery, StrId::STR_APP_GALLERY, &icon_app_gallery_48},
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
    case App::Gallery:
      next = makeUniqueNoThrow<GalleryActivity>(renderer, mappedInput);
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
  const int page = selected / PER_PAGE;
  const int pages = (APP_COUNT + PER_PAGE - 1) / PER_PAGE;
  if ((swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) && page + 1 < pages) {
    select((page + 1) * PER_PAGE);
    return;
  }
  if ((swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) && page > 0) {
    select((page - 1) * PER_PAGE);
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
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width) + metrics.contentSidePadding),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x + metrics.contentSidePadding)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int pages = (APP_COUNT + PER_PAGE - 1) / PER_PAGE;
  const int page = selected / PER_PAGE;
  const int16_t labelHeight = screen.target().lineHeight(theme.smallText.font);
  const int16_t pagerHeight = pages > 1 ? labelHeight : 0;
  const fui::Rect body = screen.body();
  const auto tileWidth = static_cast<int16_t>((body.width - TILE_GAP * (COLUMNS - 1)) / COLUMNS);
  // Square-ish tiles; the rows never grow past what fits above the pager.
  const auto tileHeight = static_cast<int16_t>(
      std::min<int>(tileWidth + labelHeight / 2, (body.height - pagerHeight - TILE_GAP * ROWS) / ROWS));
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const fui::Paint paper = fui::Paint::solid(fui::Color::White);
  const int first = page * PER_PAGE;
  const int last = std::min(APP_COUNT, first + PER_PAGE);
  for (int i = first; i < last; ++i) {
    const int slot = i - first;
    const fui::Rect tile{static_cast<int16_t>(body.x + (slot % COLUMNS) * (tileWidth + TILE_GAP)),
                         static_cast<int16_t>(body.y + (slot / COLUMNS) * (tileHeight + TILE_GAP)), tileWidth,
                         tileHeight};
    const bool isSelected = i == selected && !BoardConfig::hasTouch();
    if (isSelected) {
      screen.target().fill(tile, ink, theme.controlRadius);
    } else {
      screen.target().stroke(tile, ink, 2, theme.controlRadius);
    }
    // Labels may wrap to a second line ("Reading Stats").
    auto label = theme.smallText;
    label.align = fui::TextAlign::Center;
    label.inverted = isSelected;
    label.maxLines = 2;
    const char* name = I18n::getInstance().get(APPS[i].label);
    const auto textWidth = static_cast<int16_t>(tile.width - 2 * theme.spaceSm);
    const int16_t textHeight = fui::measureWrappedText(screen.target(), name, label, textWidth).height;
    const int16_t contentHeight = TILE_ICON + theme.spaceMd + textHeight;
    const fui::Rect iconRect{static_cast<int16_t>(tile.x + (tile.width - TILE_ICON) / 2),
                             static_cast<int16_t>(tile.y + (tile.height - contentHeight) / 2), TILE_ICON, TILE_ICON};
    screen.target().bitmap(iconRect, fui::bitmapFromIcon(*APPS[i].icon), fui::BitmapMode::Center,
                           isSelected ? paper : ink);
    screen.target().text(fui::Rect{static_cast<int16_t>(tile.x + theme.spaceSm),
                                   static_cast<int16_t>(iconRect.bottom() + theme.spaceMd), textWidth, textHeight},
                         name, label);
    screen.frame().hit(tile, OPEN, static_cast<int16_t>(i), fui::InputTouch);
  }
  if (pages > 1) {
    char pager[16];
    snprintf(pager, sizeof(pager), "%d / %d", page + 1, pages);
    auto text = theme.smallText;
    text.align = fui::TextAlign::Center;
    screen.target().text(fui::Rect{body.x, static_cast<int16_t>(body.bottom() - pagerHeight), body.width, pagerHeight},
                         pager, text);
  }
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

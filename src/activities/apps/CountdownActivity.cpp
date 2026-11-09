#include "CountdownActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "network/TilesCloud.h"

namespace fui = freeink::ui;

void CountdownActivity::onEnter() {
  applists::loadCountdowns(items);
  sortAndRefresh();
  UiListActivity::onEnter();
  app.on(ACTION_HEADER, &CountdownActivity::headerActionTrampoline, this);
}

int CountdownActivity::headerSlots() const { return BoardConfig::isPicco() ? 1 : 0; }

void CountdownActivity::headerActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<CountdownActivity*>(user);
  if (event.value < 0 || event.value >= self->headerSlots()) return;
  self->nav.selected = event.value;
  self->startSync();
}

void CountdownActivity::onRowAction(const fui::ActionEvent& event) {
  // Rows have nothing to open; a tap only moves the selection.
  nav.selected = event.value + headerSlots();
  requestUpdate();
}

void CountdownActivity::navigateButtons() {
  const int ringSize = listCount() + headerSlots();
  buttonNavigator.onNextPress(
      [this, ringSize] { moveSelectionTo(ButtonNavigator::nextIndex(nav.selected, ringSize)); });
  buttonNavigator.onPreviousPress(
      [this, ringSize] { moveSelectionTo(ButtonNavigator::previousIndex(nav.selected, ringSize)); });
  buttonNavigator.onNextContinuous([this, ringSize] {
    moveSelectionTo(ButtonNavigator::nextPageIndex(nav.selected, ringSize, nav.inputPageRows()));
  });
  buttonNavigator.onPreviousContinuous([this, ringSize] {
    moveSelectionTo(ButtonNavigator::previousPageIndex(nav.selected, ringSize, nav.inputPageRows()));
  });
}

int CountdownActivity::itemAt(const int row) const {
  const int index = row;
  return index >= 0 && index < static_cast<int>(items.size()) ? index : -1;
}

void CountdownActivity::startSync() {
  if (sync.running()) return;
  if (WiFi.status() == WL_CONNECTED) {
    runSync();
    return;
  }
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("CDN", "OOM: Wi-Fi screen");
    return;
  }
  ownsWifi = true;
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
      stopOwnWifi();
      requestUpdate();
      return;
    }
    runSync();
  });
}

void CountdownActivity::runSync() {
  editedDuringSync = false;
  if (!sync.start(items, tilescloud::syncCountdowns)) LOG_ERR("CDN", "Sync task not started");
  requestUpdate();
}

void CountdownActivity::stopOwnWifi() {
  if (!ownsWifi) return;
  ownsWifi = false;
  // The Picco has PSRAM, so Wi-Fi turns off in place, as in the Weather app.
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

void CountdownActivity::onExit() {
  stopOwnWifi();
  UiListActivity::onExit();
}

void CountdownActivity::activateIndex(int) {}

bool CountdownActivity::handleCustomInput() {
  std::vector<applists::CountdownItem> synced;
  tilescloud::SyncResult result;
  if (!sync.take(synced, result)) return false;
  stopOwnWifi();
  // An edit made meanwhile wins; sync again to pick up the cloud list.
  if (result == tilescloud::SyncResult::Ok && !editedDuringSync) {
    items = std::move(synced);
    if (!applists::saveCountdowns(items)) LOG_ERR("CDN", "Save failed");
    sortAndRefresh();
  }
  requestUpdate();
  return false;
}

void CountdownActivity::sortAndRefresh() {
  // Soonest date first; past dates sort before today, so move them last.
  std::sort(items.begin(), items.end(), [](const applists::CountdownItem& a, const applists::CountdownItem& b) {
    int da = 0;
    int db = 0;
    const bool known = applists::daysUntil(a, da) && applists::daysUntil(b, db);
    if (known && (da < 0) != (db < 0)) return db < 0;
    if (a.year != b.year) return a.year < b.year;
    if (a.month != b.month) return a.month < b.month;
    return a.day < b.day;
  });
  refreshRows();
}

const char* CountdownActivity::headerTitle() const { return tr(STR_APP_COUNTDOWN); }

void CountdownActivity::refreshRows() {
  texts.clear();
  texts.reserve(items.size());
  clockSet = true;
  for (const auto& item : items) {
    Row row;
    char buf[48];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", item.year, item.month, item.day);
    row.date = buf;
    int days = 0;
    if (!applists::daysUntil(item, days)) {
      clockSet = false;
    } else if (days == 0) {
      row.value = tr(STR_COUNTDOWN_TODAY);
    } else if (days > 0) {
      snprintf(buf, sizeof(buf), days == 1 ? tr(STR_COUNTDOWN_DAY_LEFT) : tr(STR_COUNTDOWN_DAYS_LEFT), days);
      row.value = buf;
    } else {
      snprintf(buf, sizeof(buf), -days == 1 ? tr(STR_COUNTDOWN_DAY_AGO) : tr(STR_COUNTDOWN_DAYS_AGO), -days);
      row.value = buf;
    }
    texts.push_back(std::move(row));
  }
}

void CountdownActivity::deleteRow(const int row) {
  const int index = itemAt(row);
  if (index < 0) return;
  items.erase(items.begin() + index);
  if (sync.running()) editedDuringSync = true;
  if (!applists::saveCountdowns(items)) LOG_ERR("CDN", "Save failed");
  refreshRows();
  const int ringSize = listCount() + headerSlots();
  if (nav.selected >= ringSize) moveSelectionTo(std::max(0, ringSize - 1));
  requestUpdate();
}

void CountdownActivity::promptDelete(const int row) {
  const int index = itemAt(row);
  if (index < 0) return;
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_ITEM_CONFIRM), items[index].title);
  if (!confirmation) {
    LOG_ERR("CDN", "OOM: delete confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation), [this, row](const ActivityResult& result) {
    if (!result.isCancelled) deleteRow(row);
  });
}

bool CountdownActivity::handleButtons() {
  // Item rows have nothing to open, so Confirm offers delete; a tap still does
  // nothing. On the Sync button Confirm syncs.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (nav.selected < headerSlots()) {
      startSync();
    } else if (itemAt(nav.selected - headerSlots()) >= 0) {
      promptDelete(nav.selected - headerSlots());
    }
    return true;
  }
  return UiListActivity::handleButtons();
}

void CountdownActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK),
      nav.selected < headerSlots() ? tr(STR_SELECT) : (itemAt(nav.selected - headerSlots()) >= 0 ? tr(STR_DELETE) : ""),
      tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CountdownActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const auto& theme = screen.theme();
  if (headerSlots() > 0) {
    // First row: the Sync button.
    const fui::Rect band = screen.takeTop(theme.rowHeight, static_cast<int16_t>(metrics.verticalSpacing));
    const char* syncStatus = sync.statusText();
    fui::ButtonProps button;
    button.label = syncStatus ? syncStatus : tr(STR_CLOUD_SYNC_NOW);
    button.action = ACTION_HEADER;
    button.value = 0;
    button.inputMask = fui::InputTouch;
    button.text = theme.bodyText;
    button.text.align = fui::TextAlign::Center;
    if (nav.selected == 0) button.state = fui::StateSelected;
    screen.button(button, band);
  }

  // Then the message row, then the items.
  auto hint = theme.smallText;
  hint.align = fui::TextAlign::Center;
  hint.maxLines = 3;
  const char* message = items.empty() ? tr(STR_COUNTDOWN_EMPTY_HINT) : clockSet ? nullptr : tr(STR_COUNTDOWN_NO_CLOCK);
  if (message) {
    const fui::Rect body = screen.body();
    const fui::Rect band = screen.takeTop(static_cast<int16_t>(screen.target().lineHeight(hint.font) * 3),
                                          static_cast<int16_t>(metrics.verticalSpacing));
    screen.target().text(
        fui::Rect{static_cast<int16_t>(body.x + 20), band.y, static_cast<int16_t>(body.width - 40), band.height},
        message, hint);
  }

  rows.clear();
  rows.reserve(listCount());
  for (size_t i = 0; i < items.size(); ++i) {
    fui::ListItem row;
    row.label = items[i].title.c_str();
    row.subtitle = texts[i].date.c_str();
    row.value = texts[i].value.empty() ? nullptr : texts[i].value.c_str();
    row.actionValue = static_cast<int16_t>(i);
    rows.push_back(row);
  }
  fui::ListProps props;
  props.items = rows.data();
  props.count = static_cast<uint16_t>(rows.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
#if FREEINK_CAP_TOUCH
  configureSwipeDelete(screen, props);
#endif
  syncListViewport(screen, props, headerSlots());
  screen.list(props);
}

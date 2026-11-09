#include "TodoActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "network/TilesCloud.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long DELETE_HOLD_MS = 1000;
}  // namespace

void TodoActivity::onEnter() {
  applists::loadTodos(items);
  UiListActivity::onEnter();
  app.on(ACTION_HEADER, &TodoActivity::headerActionTrampoline, this);
}

int TodoActivity::headerSlots() const { return BoardConfig::isPicco() ? 2 : 0; }

void TodoActivity::headerActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<TodoActivity*>(user);
  if (event.value < 0 || event.value >= self->headerSlots()) return;
  self->nav.selected = event.value;
  self->activateHeader(event.value);
}

void TodoActivity::activateHeader(const int slot) {
  if (slot == 0) {
    addItem();
  } else {
    startSync();
  }
}

void TodoActivity::onRowAction(const fui::ActionEvent& event) {
  nav.selected = event.value + headerSlots();
  activateIndex(event.value);
}

void TodoActivity::navigateButtons() {
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

void TodoActivity::startSync() {
  if (sync.running()) return;
  if (WiFi.status() == WL_CONNECTED) {
    runSync();
    return;
  }
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("TODO", "OOM: Wi-Fi screen");
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

void TodoActivity::runSync() {
  editedDuringSync = false;
  if (!sync.start(items, tilescloud::syncTodos)) LOG_ERR("TODO", "Sync task not started");
  requestUpdate();
}

void TodoActivity::stopOwnWifi() {
  if (!ownsWifi) return;
  ownsWifi = false;
  // The Picco has PSRAM, so Wi-Fi turns off in place, as in the Weather app.
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

void TodoActivity::onExit() {
  stopOwnWifi();
  UiListActivity::onExit();
}

bool TodoActivity::handleCustomInput() {
  std::vector<applists::TodoItem> synced;
  tilescloud::SyncResult result;
  if (!sync.take(synced, result)) return false;
  stopOwnWifi();
  // An edit made meanwhile wins; sync again to pick up the cloud list.
  if (result == tilescloud::SyncResult::Ok && !editedDuringSync) {
    items = std::move(synced);
    save();
  }
  requestUpdate();
  return false;
}

const char* TodoActivity::headerTitle() const { return tr(STR_APP_TODO); }

int TodoActivity::listCount() const { return addRows() + static_cast<int>(items.size()) + (hasDone() ? 1 : 0); }

int TodoActivity::itemAt(const int row) const {
  const int index = row - addRows();
  return index >= 0 && index < static_cast<int>(items.size()) ? index : -1;
}

bool TodoActivity::hasDone() const {
  return std::any_of(items.begin(), items.end(), [](const applists::TodoItem& item) { return item.done; });
}

void TodoActivity::save() {
  if (sync.running()) editedDuringSync = true;
  if (!applists::saveTodos(items)) LOG_ERR("TODO", "Save failed");
}

void TodoActivity::activateIndex(const int index) {
  if (addRows() > 0 && index == 0) {
    addItem();
    return;
  }
  const int item = itemAt(index);
  if (item >= 0) {
    items[item].done = !items[item].done;
  } else {
    items.erase(std::remove_if(items.begin(), items.end(), [](const applists::TodoItem& t) { return t.done; }),
                items.end());
    moveSelectionTo(0);
  }
  save();
  requestUpdate();
}

void TodoActivity::deleteRow(const int row) {
  const int item = itemAt(row);
  if (item < 0) return;
  items.erase(items.begin() + item);
  save();
  const int ringSize = listCount() + headerSlots();
  if (nav.selected >= ringSize) moveSelectionTo(ringSize - 1);
  requestUpdate();
}

void TodoActivity::promptDelete(const int row) {
  const int item = itemAt(row);
  if (item < 0) return;
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_ITEM_CONFIRM), items[item].text);
  if (!confirmation) {
    LOG_ERR("TODO", "OOM: delete confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation), [this, row](const ActivityResult& result) {
    if (!result.isCancelled) deleteRow(row);
  });
}

bool TodoActivity::handleButtons() {
  // The hold fires mid-press and swallows its release, so it never also ticks the row.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, DELETE_HOLD_MS)) {
    if (nav.selected >= headerSlots()) promptDelete(nav.selected - headerSlots());
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int selected = nav.selected;
    if (selected >= 0 && selected < headerSlots()) {
      activateHeader(selected);
    } else if (selected - headerSlots() < listCount()) {
      activateIndex(selected - headerSlots());
    }
    return true;
  }
  return UiListActivity::handleButtons();
}

void TodoActivity::addItem() {
  if (items.size() >= applists::MAX_TODOS) return;
  app.clearTapFlash();
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TODO_ADD), "",
                                                           applists::MAX_TEXT, InputType::Text);
  if (!keyboard) {
    LOG_ERR("TODO", "OOM: keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto& text = std::get<KeyboardResult>(result.data).text;
    if (text.empty()) return;
    items.push_back({text, false});
    save();
    requestUpdate();
  });
}

void TodoActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const auto& theme = screen.theme();
  auto hint = theme.smallText;
  hint.align = fui::TextAlign::Center;
  hint.maxLines = 3;
  const char* addLabel = items.size() < applists::MAX_TODOS ? tr(STR_TODO_ADD) : tr(STR_TODO_FULL);

  if (headerSlots() > 0) {
    // First row: Add on the left, Sync on the right.
    const fui::Rect band = screen.takeTop(theme.rowHeight, static_cast<int16_t>(metrics.verticalSpacing));
    const int16_t gap = static_cast<int16_t>(metrics.verticalSpacing);
    const int16_t half = static_cast<int16_t>((band.width - gap) / 2);
    const char* syncStatus = sync.statusText();
    const char* labels[2] = {addLabel, syncStatus ? syncStatus : tr(STR_CLOUD_SYNC_NOW)};
    for (int slot = 0; slot < 2; ++slot) {
      fui::ButtonProps button;
      button.label = labels[slot];
      button.action = ACTION_HEADER;
      button.value = static_cast<int16_t>(slot);
      button.inputMask = fui::InputTouch;
      button.text = theme.bodyText;
      button.text.bold = slot == 0;
      button.text.align = fui::TextAlign::Center;
      // Outlined, rounded button, as on the book details page.
      button.styles = theme.button;
      button.styles.normal.border = fui::Paint::solid(fui::Color::Black);
      button.styles.normal.borderWidth = 1;
      button.radius = theme.controlRadius;
      if (nav.selected == slot) button.state = fui::StateSelected;
      const int16_t x = static_cast<int16_t>(slot == 0 ? band.x : band.x + band.width - half);
      screen.button(button, fui::Rect{x, band.y, half, band.height});
    }
    // Second row: the empty-list message.
    if (items.empty()) {
      const fui::Rect message = screen.takeTop(static_cast<int16_t>(screen.target().lineHeight(hint.font) * 3),
                                               static_cast<int16_t>(metrics.verticalSpacing));
      screen.target().text(fui::Rect{static_cast<int16_t>(message.x + 20), message.y,
                                     static_cast<int16_t>(message.width - 40), message.height},
                           tr(STR_TODO_EMPTY_HINT), hint);
    }
  }

  rows.clear();
  rows.reserve(listCount());
  if (addRows() > 0) {
    fui::ListItem add;
    add.label = addLabel;
    add.bold = true;
    add.actionValue = 0;
    rows.push_back(add);
  }
  for (size_t i = 0; i < items.size(); ++i) {
    fui::ListItem row;
    row.label = items[i].text.c_str();
    row.toggle = true;
    row.toggleChecked = items[i].done;
    row.actionValue = static_cast<int16_t>(i + addRows());
    rows.push_back(row);
  }
  if (hasDone()) {
    fui::ListItem clear;
    clear.label = tr(STR_TODO_CLEAR_DONE);
    clear.actionValue = static_cast<int16_t>(rows.size());
    rows.push_back(clear);
  }

  fui::ListProps props;
  props.items = rows.data();
  props.count = static_cast<uint16_t>(rows.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = theme.bodyText;
  props.labelText.maxLines = 3;
#if FREEINK_CAP_TOUCH
  configureSwipeDelete(screen, props);
#endif
  syncListViewport(screen, props, headerSlots());
  screen.list(props);

  // Without the header, the empty-list message sits under the Add row.
  if (headerSlots() == 0 && items.empty()) {
    const fui::Rect body = screen.body();
    screen.target().text(fui::Rect{static_cast<int16_t>(body.x + 20), static_cast<int16_t>(body.y + 40),
                                   static_cast<int16_t>(body.width - 40), 90},
                         tr(STR_TODO_EMPTY_HINT), hint);
  }
}

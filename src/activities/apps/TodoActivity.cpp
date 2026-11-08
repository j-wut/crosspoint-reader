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
}

int TodoActivity::syncRows() const { return BoardConfig::isPicco() ? 1 : 0; }

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

int TodoActivity::listCount() const { return 1 + syncRows() + static_cast<int>(items.size()) + (hasDone() ? 1 : 0); }

int TodoActivity::itemAt(const int row) const {
  const int index = row - 1 - syncRows();
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
  if (index == 0) {
    addItem();
    return;
  }
  if (syncRows() > 0 && index == 1) {
    startSync();
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
  if (nav.selected >= listCount()) moveSelectionTo(listCount() - 1);
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
    promptDelete(nav.selected);
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

  rows.clear();
  rows.reserve(listCount());
  fui::ListItem add;
  add.label = items.size() < applists::MAX_TODOS ? tr(STR_TODO_ADD) : tr(STR_TODO_FULL);
  add.bold = true;
  add.actionValue = 0;
  rows.push_back(add);
  if (syncRows() > 0) {
    fui::ListItem syncRow;
    syncRow.label = tr(STR_CLOUD_SYNC_NOW);
    syncRow.value = sync.statusText();
    syncRow.actionValue = 1;
    rows.push_back(syncRow);
  }
  for (size_t i = 0; i < items.size(); ++i) {
    fui::ListItem row;
    row.label = items[i].text.c_str();
    row.toggle = true;
    row.toggleChecked = items[i].done;
    row.actionValue = static_cast<int16_t>(i + 1 + syncRows());
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
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 3;
#if FREEINK_CAP_TOUCH
  configureSwipeDelete(screen, props);
#endif
  syncListViewport(screen, props);
  screen.list(props);

  if (items.empty()) {
    auto hint = screen.theme().smallText;
    hint.align = fui::TextAlign::Center;
    hint.maxLines = 3;
    const fui::Rect body = screen.body();
    screen.target().text(fui::Rect{static_cast<int16_t>(body.x + 20), static_cast<int16_t>(body.y + 40),
                                   static_cast<int16_t>(body.width - 40), 90},
                         tr(STR_TODO_EMPTY_HINT), hint);
  }
}

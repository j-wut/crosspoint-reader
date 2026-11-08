#include "CountdownActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "network/TilesCloud.h"

namespace fui = freeink::ui;

void CountdownActivity::onEnter() {
  applists::loadCountdowns(items);
  sortAndRefresh();
  UiListActivity::onEnter();
}

int CountdownActivity::syncRows() const { return BoardConfig::isPicco() ? 1 : 0; }

int CountdownActivity::itemAt(const int row) const {
  const int index = row - syncRows();
  return index >= 0 && index < static_cast<int>(items.size()) ? index : -1;
}

void CountdownActivity::startSync() {
  if (sync.running()) return;
  editedDuringSync = false;
  if (!sync.start(items, tilescloud::syncCountdowns)) LOG_ERR("CDN", "Sync task not started");
  requestUpdate();
}

void CountdownActivity::activateIndex(const int index) {
  if (syncRows() > 0 && index == 0) startSync();
}

bool CountdownActivity::handleCustomInput() {
  std::vector<applists::CountdownItem> synced;
  tilescloud::SyncResult result;
  if (!sync.take(synced, result)) return false;
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
  if (nav.selected >= listCount()) moveSelectionTo(std::max(0, listCount() - 1));
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
  // nothing. On the sync row Confirm syncs.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (itemAt(nav.selected) >= 0) {
      promptDelete(nav.selected);
    } else {
      activateIndex(nav.selected);
    }
    return true;
  }
  return UiListActivity::handleButtons();
}

void CountdownActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), itemAt(nav.selected) >= 0 ? tr(STR_DELETE) : (syncRows() > 0 ? tr(STR_SELECT) : ""), tr(STR_DIR_UP),
      tr(STR_DIR_DOWN));
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

  auto hint = screen.theme().smallText;
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
  if (syncRows() > 0) {
    fui::ListItem syncRow;
    syncRow.label = tr(STR_CLOUD_SYNC_NOW);
    syncRow.value = sync.statusText();
    syncRow.actionValue = 0;
    rows.push_back(syncRow);
  }
  for (size_t i = 0; i < items.size(); ++i) {
    fui::ListItem row;
    row.label = items[i].title.c_str();
    row.subtitle = texts[i].date.c_str();
    row.value = texts[i].value.empty() ? nullptr : texts[i].value.c_str();
    row.actionValue = static_cast<int16_t>(i + syncRows());
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
  syncListViewport(screen, props);
  screen.list(props);
}

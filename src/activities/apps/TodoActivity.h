#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "apps/AppLists.h"
#include "network/CloudSyncJob.h"

// To-do list: tap a row to tick it off, swipe left to delete (buttons: hold
// Confirm). The first row adds an item with the keyboard; the web page edits
// the same list.
class TodoActivity final : public UiListActivity {
 public:
  TodoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("Todo", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;

 private:
  // BOOX Tiles cloud sync, only when the sync row is pressed (Picco only). It
  // runs in the background; the list stays usable meanwhile.
  bool handleCustomInput() override;
  // Without Wi-Fi, the Wi-Fi screen comes first (it joins a saved network
  // or lets the user pick one); the sync starts once connected.
  void startSync();
  void runSync();
  void stopOwnWifi();
  bool ownsWifi = false;
  CloudSyncJob<applists::TodoItem> sync;
  bool editedDuringSync = false;
  // Rows: 0 adds an item, then the items, a row that clears done items, and
  // last the sync row (Picco).
  int syncRows() const;
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  bool handleButtons() override;
#if FREEINK_CAP_TOUCH
  bool canSwipeDelete(int index) const override { return itemAt(index) >= 0; }
  void swipeDelete(int index) override { deleteRow(index); }
#endif
  void deleteRow(int row);
  void promptDelete(int row);
  int itemAt(int row) const;
  bool hasDone() const;
  void addItem();
  void save();

  std::vector<applists::TodoItem> items;
  std::vector<freeink::ui::ListItem> rows;
};

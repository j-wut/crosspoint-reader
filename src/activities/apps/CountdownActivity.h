#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "apps/AppLists.h"
#include "network/CloudSyncJob.h"

// Days left until each saved date, soonest first. Dates are added on the web
// page; swipe left on a row to delete it (buttons: Confirm).
class CountdownActivity final : public UiListActivity {
 public:
  CountdownActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("Countdown", renderer, mappedInput) {}

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
  CloudSyncJob<applists::CountdownItem> sync;
  bool editedDuringSync = false;
  // Row 0 is the sync row on the Picco; the items follow.
  int syncRows() const;
  int itemAt(int row) const;
  struct Row {
    std::string value;
    std::string date;
  };
  int listCount() const override { return syncRows() + static_cast<int>(items.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  bool handleButtons() override;
  void drawFooter() override;
#if FREEINK_CAP_TOUCH
  bool canSwipeDelete(int index) const override { return itemAt(index) >= 0; }
  void swipeDelete(int index) override { deleteRow(index); }
#endif
  void deleteRow(int index);
  void promptDelete(int index);
  void refreshRows();
  void sortAndRefresh();

  std::vector<applists::CountdownItem> items;
  std::vector<Row> texts;
  std::vector<freeink::ui::ListItem> rows;
  bool clockSet = false;
};

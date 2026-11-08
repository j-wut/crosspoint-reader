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

 private:
  // BOOX Tiles cloud sync on open (Picco only), like the stock app. It runs
  // in the background; the list stays usable meanwhile.
  bool handleCustomInput() override;
  CloudSyncJob<applists::CountdownItem> sync;
  bool editedDuringSync = false;
  std::string syncingTitle;
  struct Row {
    std::string value;
    std::string date;
  };
  int listCount() const override { return static_cast<int>(items.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int) override {}
  const char* headerTitle() const override;
  bool handleButtons() override;
  void drawFooter() override;
#if FREEINK_CAP_TOUCH
  bool canSwipeDelete(int index) const override { return index >= 0 && index < listCount(); }
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

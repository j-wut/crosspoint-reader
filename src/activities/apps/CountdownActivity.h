#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "apps/AppLists.h"

// Days left until each saved date, soonest first. Dates are added on the web
// page; swipe left on a row to delete it.
class CountdownActivity final : public UiListActivity {
 public:
  CountdownActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("Countdown", renderer, mappedInput) {}

  void onEnter() override;

 private:
  struct Row {
    std::string value;
    std::string date;
  };
  int listCount() const override { return static_cast<int>(items.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int) override {}
  const char* headerTitle() const override;
#if FREEINK_CAP_TOUCH
  bool canSwipeDelete(int index) const override { return index >= 0 && index < listCount(); }
  void swipeDelete(int index) override;
#endif
  void refreshRows();

  std::vector<applists::CountdownItem> items;
  std::vector<Row> texts;
  std::vector<freeink::ui::ListItem> rows;
  bool clockSet = false;
};

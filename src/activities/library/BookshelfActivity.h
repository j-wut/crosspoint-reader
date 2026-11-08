#pragma once

#include <array>
#include <string>

#include "activities/Activity.h"
#include "components/HomeCoverCache.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Tiles theme Library: every indexed book as a cover with its title and
// reading progress, newest arrival first, six to a page. Tap opens, hold
// offers delete, and "List" opens the full Library list (search and sort).
class BookshelfActivity final : public Activity, private UiAppHost {
 public:
  BookshelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Bookshelf", renderer, mappedInput), UiAppHost(renderer), coverCache(renderer) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int COLUMNS = 3;
  static constexpr int ROWS = 2;
  static constexpr int PER_PAGE = COLUMNS * ROWS;
  static_assert(PER_PAGE <= HomeCoverCache::MAX_COVERS);

  struct Cell {
    std::string path;
    std::string title;
    std::string thumbBase;  // book's thumb path without the height suffix
    std::string coverPath;  // thumb at the drawn height, when it exists
    int progress = -1;
    bool hasThumb = false;
    bool thumbTried = false;
  };

  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void drawCell(UiScreen& screen, int slot, freeink::ui::Rect rect);
  bool openIndex();
  void loadPage(int newPage);
  void refreshThumbs();
  bool generateMissingThumb();
  void openCell(int slot);
  void promptDelete(int slot);
  void select(int index);

  HomeCoverCache coverCache;
  ButtonNavigator buttonNavigator;
  std::array<Cell, PER_PAGE> cells;
  int cellCount = 0;
  int bookCount = 0;
  int page = 0;
  int selected = 0;  // flat index on the page, buttons only
  int thumbHeight = 0;
  bool thumbHeightChanged = false;
  int pending = -1;
  bool pendingLong = false;
};

#pragma once

#include <array>
#include <string>
#include <vector>

#include "HomeCoverCache.h"
#include "RecentBooksStore.h"
#include "UiAppHost.h"

// Tiles home: a row of three recent covers with the selected book's title and
// progress, above a dock of Books, Transfer, Apps and Settings tiles.
// Selection is one flat index: covers first, then the dock tiles.
class TilesHomeUi final : public UiAppHost {
 public:
  static constexpr int MAX_BOOKS = 3;
  static constexpr int DOCK_COUNT = 4;
  enum class Dock : uint8_t { Books, Transfer, Apps, Settings };
  static_assert(MAX_BOOKS <= HomeCoverCache::MAX_COVERS);

  explicit TilesHomeUi(GfxRenderer& renderer);
  void begin(const std::vector<RecentBook>& books);
  void refreshCoverPaths();
  void setSelection(int selection);
  int shownBookIndex() const { return shownBook; }
  // Flat index of a tapped cover or tile on touch release, else -1.
  int selectedAction(const MappedInputManager& input);
  // Exact generation height, recorded during draw (see CoverGridHomeUi).
  int thumbHeightFor() const;
  bool takeThumbHeightChanged();

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void drawCovers(UiScreen& screen, freeink::ui::Rect rect);
  void drawBookInfo(UiScreen& screen, freeink::ui::Rect rect);
  void drawEmpty(UiScreen& screen, freeink::ui::Rect rect);
  void drawDock(UiScreen& screen, freeink::ui::Rect rect);
  void loadProgress();
  void noteThumbHeight(int slotHeight);

  HomeCoverCache coverCache;
  GfxRenderer& renderer;
  const std::vector<RecentBook>* books = nullptr;
  std::array<std::string, MAX_BOOKS> coverPaths;
  std::array<bool, MAX_BOOKS> hasThumb{};
  int thumbHeight = 0;
  bool thumbHeightChanged = false;
  int selected = 0;
  int shownBook = 0;  // book whose title and progress are shown
  int pending = -1;
  int progress = -1;
  char progressText[12]{};
};

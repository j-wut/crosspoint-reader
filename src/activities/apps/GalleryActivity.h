#pragma once

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "apps/Wallpaper.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Images in /images (BMP, PNG, JPEG, BOOX .opg) as a grid of previews, six to
// a page, like the stock Picco Gallery. Open one to view it or set it as the
// sleep, reading sleep or power-off screen. The BOOX Tiles app and the web
// page add images here. Buttons: press steps one image, hold jumps a page.
class GalleryActivity final : public Activity, private UiAppHost {
 public:
  GalleryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Gallery", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int COLUMNS = 3;
  static constexpr int ROWS = 2;
  static constexpr int PER_PAGE = COLUMNS * ROWS;

  struct Cell {
    int item = -1;
    wallpaper::Thumb thumb;  // empty when the preview failed
    std::string cachePath;
    bool needsSave = false;  // made from the image; cache file not written yet
  };

  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void drawCell(UiScreen& screen, int slot, freeink::ui::Rect rect);
  int itemCount() const;
  void reload();
  void loadPage(int newPage);
  void makePreviews();
  bool saveOnePreview();
  void activate(int item);
  void openImage(int index);
  void runAction(int image, int action);
  void select(int item);

  ButtonNavigator buttonNavigator;
  std::vector<std::string> images;
  std::vector<std::string> names;
  std::array<Cell, PER_PAGE> cells;
  int cellCount = 0;
  int page = 0;
  int selected = 0;    // flat index on the page, buttons only
  int holdAnchor = 0;  // image a held button pages from
  int thumbW = 0;      // preview size, set by the render task
  int thumbH = 0;
  bool thumbSizeChanged = false;
  bool previewsReady = false;             // the grid is shown only once all its previews are made
  std::atomic<bool> loadingShown{false};  // set by the render task once the loading ring is up
  int pending = -1;
};

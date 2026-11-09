#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Wallpapers in /images (BMP, PNG, JPEG, BOOX .opg), like the stock Picco
// Gallery: open one to view it or set it as the sleep, reading sleep or
// power-off screen. The BOOX Tiles app and the web page add images here.
class GalleryActivity final : public UiListActivity {
 public:
  GalleryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("Gallery", renderer, mappedInput) {}

  void onEnter() override;

 private:
  // Rows: the images, then "Random sleep image" and, when one is set,
  // "Turn off reading sleep screen".
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void reload();
  void openImage(int index);
  void runAction(int image, int action);

  std::vector<std::string> images;
  std::vector<std::string> names;
  std::vector<freeink::ui::ListItem> rows;
  bool hasReading = false;
};

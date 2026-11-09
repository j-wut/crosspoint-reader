#pragma once

#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Back finishes this activity (returning to the caller, e.g. the Gallery)
  // instead of opening the file browser.
  void setReturnToCaller() { returnToCaller = true; }

 private:
  void loadSiblingImages();
  void doSetSleepCover();
  bool canSetSleepCover() const;
  bool renderImage();
  bool saveJpegSleepCover();
  void drawOverlay();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  bool returnToCaller = false;
  bool overlayShown = false;  // touch boards: tap shows a top bar and the file name
};

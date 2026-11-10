#pragma once

#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "apps/ScreenCache.h"
#include "components/themes/BaseTheme.h"

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
  bool showCachedScreen(screencache::Reader& cache, bool withOverlay = false);
  void drawHints() const;
  bool saveJpegSleepCover();
  void drawOverlay();
  void drawOverlayContent();
  void drawOverlayGrayPlane(bool absolute);

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  bool returnToCaller = false;
  bool overlayShown = false;  // touch boards: tap shows the top and bottom bars
  bool keepOverlay = false;   // redraw the bars on the next image
  Rect prevArrow{};
  Rect nextArrow{};
  std::string screenKey;  // cache key of the image on screen; empty when it cannot be cached
};

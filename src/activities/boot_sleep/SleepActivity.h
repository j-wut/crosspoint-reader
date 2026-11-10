#pragma once
#include <I18n.h>

#include <string>

#include "activities/Activity.h"
#include "apps/ScreenCache.h"
#include "apps/Wallpaper.h"

class Bitmap;
class HalFile;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false,
                         bool powerOff = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout), powerOff(powerOff) {}
  void onEnter() override;
  // True when the sleep screen on the panel has grays. The frame buffer then
  // holds a gray plane, so a B/W refresh of it would show the wrong image.
  static bool screenHasGray();

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  bool renderCustomSleepBmp(const std::string& path) const;
  bool renderCachedSleepScreen(const std::string& key) const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool preserveBackground = false,
                               screencache::Writer* capture = nullptr) const;
  bool renderSleepOverlayFile(HalFile& file, const char* pathForLog) const;
  bool renderTransparentOverlayPng(const std::string& path) const;
  bool renderSleepOverlayPath(const std::string& path) const;
  void renderLastScreenSleepScreen() const;
  void renderTransparentCustomSleepScreen() const;
  void renderBlankSleepScreen() const;
  bool galleryWallpaper(wallpaper::Kind& kind, std::string& path) const;
  bool renderGalleryWallpaper(bool frameWasInverted) const;
  bool renderGalleryImageScreen(wallpaper::Kind kind, const std::string& path) const;

  const char* sleepPopupText() const { return powerOff ? tr(STR_SHUTTING_DOWN) : tr(STR_ENTERING_SLEEP); }

  bool fromTimeout = false;
  bool powerOff = false;
  // Cover + Transparent: the cover under the overlay, and the same cover while its gray planes are redrawn.
  mutable const Bitmap* overlayCover = nullptr;
  mutable const Bitmap* overlayGrayCover = nullptr;
};

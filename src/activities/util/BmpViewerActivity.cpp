#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <BoardConfig.h>
#include <Epub/converters/JpegToFramebufferConverter.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "activities/util/ConfirmationActivity.h"
#include "apps/ImageScreen.h"
#include "apps/Wallpaper.h"
#include "components/HeaderBackTapTarget.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char CUSTOM_SLEEP_ROOT_BMP[] = "/sleep.bmp";
constexpr char CUSTOM_SLEEP_TMP_BMP[] = "/sleep.bmp.tmp";
constexpr char JPEG_SLEEP_TMP_PXC[] = "/sleep.bmp.pxc.tmp";
constexpr char TRANSPARENT_SLEEP_ROOT_BMP[] = "/sleep-overlay.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_PNG[] = "/sleep-overlay.png";
constexpr size_t COPY_BUFFER_SIZE = 2048;
constexpr uint8_t GRAYSCALE_PALETTE[] = {0, 0, 0, 0, 85, 85, 85, 0, 170, 170, 170, 0, 255, 255, 255, 0};
constexpr int OVERLAY_ARROW_H = 44;
bool overlaySleepMode() {
  const uint8_t mode = SETTINGS.sleepScreen;
  return mode == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM ||
         mode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_TRANSPARENT;
}
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  const auto name = makeUniqueNoThrow<char[]>(500);
  if (!name) {
    LOG_ERR("BMP", "OOM: sibling filename buffer");
    return;
  }
  size_t imageCount = 0;
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name.get(), 500);
    if (name[0] != '.' && wallpaper::isImage(std::string_view{name.get()})) ++imageCount;
  }
  dir.rewindDirectory();
  siblingImages.reserve(imageCount);

  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name.get(), 500);
      if (name[0] != '.') {
        std::string fname(name.get());
        if (wallpaper::isImage(fname)) {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);

  const auto image = std::find(siblingImages.begin(), siblingImages.end(), fileName);
  if (image != siblingImages.end()) {
    currentImageIndex = static_cast<int>(image - siblingImages.begin());
  }
}

bool BmpViewerActivity::canSetSleepCover() const {
  return FsHelpers::hasBmpExtension(filePath) || FsHelpers::hasJpgExtension(filePath) ||
         (overlaySleepMode() && FsHelpers::hasPngExtension(filePath));
}

void BmpViewerActivity::drawHints() const {
  const bool hasPrevious = siblingImages.size() > 1 && currentImageIndex > 0;
  const bool hasNext = siblingImages.size() > 1 && currentImageIndex != -1 &&
                       currentImageIndex < static_cast<int>(siblingImages.size()) - 1;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), canSetSleepCover() ? tr(STR_SET_SLEEP_COVER) : "",
                                            hasPrevious ? "<" : "", hasNext ? ">" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// Same sequence as a first draw, with the image layers read from the cache.
// The bars go into every layer, so the grays under them stay gray.
bool BmpViewerActivity::showCachedScreen(screencache::Reader& cache, bool withOverlay) {
  withOverlay = withOverlay || keepOverlay;
  const unsigned long start = millis();
  if (!cache.readLayer(0, renderer)) return false;
  drawHints();
  if (withOverlay) drawOverlayContent();
  const unsigned long bwLoaded = millis();
  if (!cache.gray()) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    LOG_INF("GAL", "cached screen %s: load %lu ms, refresh %lu ms", filePath.c_str(), bwLoaded - start,
            millis() - bwLoaded);
    return true;
  }

  const bool absolute = cache.absolute();
  // False lets the normal path draw the image or show the error.
  if (absolute && !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) return false;
  if (!absolute) renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  const unsigned long base = millis();
  bool ready = true;
  int layer = 1;
  for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    if (!cache.readLayer(layer++, renderer)) {
      ready = false;
      break;
    }
    renderer.setRenderMode(mode);
    drawHints();
    if (withOverlay) drawOverlayGrayPlane(absolute);
    if (mode == GfxRenderer::GRAYSCALE_LSB) {
      renderer.copyGrayscaleLsbBuffers();
    } else {
      renderer.copyGrayscaleMsbBuffers();
    }
  }
  const unsigned long grayLoaded = millis();
  if (ready) renderer.displayGrayBuffer();
  const unsigned long grayShown = millis();

  // Rebuild the BW framebuffer for popups and subsequent differential updates.
  renderer.setRenderMode(GfxRenderer::BW);
  if (!cache.readLayer(0, renderer)) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_FILE_OPEN_FAILED));
    ready = false;
  }
  drawHints();
  if (withOverlay) drawOverlayContent();
  renderer.cleanupGrayscaleWithFrameBuffer();
  if (!ready) renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  LOG_INF("GAL", "cached gray screen %s: B/W load %lu ms, base refresh %lu ms, gray load %lu ms, gray refresh %lu ms",
          filePath.c_str(), bwLoaded - start, base - bwLoaded, grayLoaded - base, grayShown - grayLoaded);
  return true;
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();
  overlayShown = false;
  HeaderBackTapTarget::clear();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  const std::string cacheKey = imagescreen::cacheKey(filePath, renderer);
  screenKey = cacheKey;
  {
    screencache::Reader cache;
    if (!cacheKey.empty() && cache.open(cacheKey, renderer) && showCachedScreen(cache)) return;
  }
  if (!cacheKey.empty()) LOG_INF("GAL", "no cached screen for %s; drawing it", filePath.c_str());

  // Drawn into the screen cache first, then shown from it like a cached screen,
  // so the sleep screen shows exactly the same layers.
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);
  screencache::Reader built;
  if (imagescreen::build(renderer, filePath, cacheKey) && built.open(cacheKey, renderer) && showCachedScreen(built)) {
    return;
  }
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2,
                            FsHelpers::hasBmpExtension(filePath) ? tr(STR_INVALID_BMP_FILE) : tr(STR_FILE_OPEN_FAILED));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, "", "", "");
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

bool BmpViewerActivity::saveJpegSleepCover() {
  if (Storage.exists(JPEG_SLEEP_TMP_PXC) && !Storage.remove(JPEG_SLEEP_TMP_PXC)) {
    LOG_ERR("BMP", "Failed to remove previous sleep image pixels");
    return false;
  }
  bool keepBmpTemp = false;
  const ScopedCleanup cleanup{[&keepBmpTemp]() {
    Storage.remove(JPEG_SLEEP_TMP_PXC);
    if (!keepBmpTemp) Storage.remove(CUSTOM_SLEEP_TMP_BMP);
  }};

  JpegToFramebufferConverter decoder;
  ImageDimensions dimensions;
  if (!decoder.getDimensions(filePath, dimensions)) return false;
  auto config = imagescreen::fitJpeg(dimensions, renderer);
  if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP &&
      (dimensions.width > renderer.getScreenWidth() || dimensions.height > renderer.getScreenHeight())) {
    const float ratio = static_cast<float>(dimensions.width) / dimensions.height;
    const float screenRatio = static_cast<float>(renderer.getScreenWidth()) / renderer.getScreenHeight();
    config.x = config.y = 0;
    config.maxWidth = renderer.getScreenWidth();
    config.maxHeight = renderer.getScreenHeight();
    if (ratio > screenRatio) {
      config.sourceCropX = 1.0f - screenRatio / ratio;
    } else {
      config.sourceCropY = 1.0f - ratio / screenRatio;
    }
  }
  config.cachePath = JPEG_SLEEP_TMP_PXC;
  renderer.clearScreen();
  bool cacheWritten;
  if (!decoder.decodeToFramebuffer(filePath, renderer, config, cacheWritten) || !cacheWritten) {
    LOG_ERR("BMP", "Failed to decode sleep image pixels");
    return false;
  }

  HalFile input, output;
  if (!Storage.openFileForRead("BMP", JPEG_SLEEP_TMP_PXC, input)) return false;
  uint16_t width, height;
  if (input.read(&width, sizeof(width)) != sizeof(width) || input.read(&height, sizeof(height)) != sizeof(height) ||
      width != config.maxWidth || height != config.maxHeight) {
    LOG_ERR("BMP", "Invalid sleep image pixels");
    return false;
  }
  const size_t pixelRowBytes = (width + 3) / 4;
  const size_t bmpRowBytes = (width + 15) / 16 * 4;
  if (input.size() != 4 + pixelRowBytes * height) {
    LOG_ERR("BMP", "Incomplete sleep image pixels");
    return false;
  }
  auto row = makeUniqueNoThrow<uint8_t[]>(bmpRowBytes);
  if (!row) {
    LOG_ERR("BMP", "OOM: sleep image row");
    return false;
  }
  if (!Storage.openFileForWrite("BMP", CUSTOM_SLEEP_TMP_BMP, output)) return false;
  BmpHeader header;
  createBmpHeader(&header, width, height, BmpRowOrder::TopDown);
  header.infoHeader.biBitCount = 2;
  header.infoHeader.biClrUsed = header.infoHeader.biClrImportant = 4;
  header.infoHeader.biSizeImage = bmpRowBytes * height;
  header.fileHeader.bfOffBits = sizeof(header) - sizeof(header.colors) + sizeof(GRAYSCALE_PALETTE);
  header.fileHeader.bfSize = header.fileHeader.bfOffBits + header.infoHeader.biSizeImage;
  if (output.write(&header, sizeof(header) - sizeof(header.colors)) != sizeof(header) - sizeof(header.colors) ||
      output.write(GRAYSCALE_PALETTE, sizeof(GRAYSCALE_PALETTE)) != sizeof(GRAYSCALE_PALETTE)) {
    LOG_ERR("BMP", "Failed to write sleep image header");
    return false;
  }
  for (int y = 0; y < height; ++y) {
    if (input.read(row.get(), pixelRowBytes) != pixelRowBytes || output.write(row.get(), bmpRowBytes) != bmpRowBytes) {
      LOG_ERR("BMP", "Failed to write sleep image row %d", y);
      return false;
    }
  }
  if (!output.close()) {
    LOG_ERR("BMP", "Failed to close sleep image");
    return false;
  }
  if (!Storage.replaceFile(CUSTOM_SLEEP_TMP_BMP, CUSTOM_SLEEP_ROOT_BMP)) {
    // ponytail: FAT replacement is not atomic; keep the complete temp for recovery.
    keepBmpTemp = true;
    LOG_ERR("BMP", "Failed to publish sleep image");
    return false;
  }
  return true;
}

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  const bool jpeg = FsHelpers::hasJpgExtension(filePath);
  const bool transparentMode = !jpeg && overlaySleepMode();
  if (!canSetSleepCover()) return;

  const char* destination =
      transparentMode ? (FsHelpers::hasPngExtension(filePath) ? TRANSPARENT_SLEEP_ROOT_PNG : TRANSPARENT_SLEEP_ROOT_BMP)
                      : CUSTOM_SLEEP_ROOT_BMP;
  bool success = filePath == destination;

  if (jpeg) {
    success = saveJpegSleepCover();
  } else if (!success) {
    auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUFFER_SIZE);
    if (!buffer) {
      LOG_ERR("BMP", "OOM: sleep cover copy buffer");
    } else {
      // Copy beside the target and swap in only a complete image, so a failed
      // copy keeps the previous sleep cover instead of a truncated one.
      const std::string tmp = std::string(destination) + ".tmp";
      HalFile inFile, outFile;
      if (Storage.openFileForRead("BMP", filePath, inFile) && Storage.openFileForWrite("BMP", tmp, outFile)) {
        int bytesRead;
        success = true;
        while ((bytesRead = inFile.read(buffer.get(), COPY_BUFFER_SIZE)) > 0) {
          if (outFile.write(buffer.get(), static_cast<size_t>(bytesRead)) != static_cast<size_t>(bytesRead)) {
            success = false;
            break;
          }
        }
        if (bytesRead < 0) success = false;
        outFile.close();
        success = success && Storage.replaceFile(tmp.c_str(), destination);
        if (!success) Storage.remove(tmp.c_str());
      }
    }
  }

  if (success) {
    if (!transparentMode) SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }

  delay(1000);
  onEnter();
}

// Top bar: back arrow and file name. Bottom bar: previous / next arrows and
// "image / images".
void BmpViewerActivity::drawOverlayContent() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int barHeight = metrics.topPadding + metrics.headerHeight;
  const std::string name = filePath.substr(filePath.find_last_of('/') + 1);
  renderer.fillRect(0, 0, width, barHeight, false);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, name.c_str());

  constexpr int ARROW_W = 72;
  constexpr int ARROW_H = OVERLAY_ARROW_H;
  const int footHeight = ARROW_H + 2 * metrics.verticalSpacing;
  const int footY = height - footHeight;
  renderer.fillRect(0, footY, width, footHeight, false);
  renderer.drawLine(0, footY, width - 1, footY, 2, true);
  const int arrowY = footY + metrics.verticalSpacing;
  const int side = metrics.contentSidePadding;
  const int count = static_cast<int>(siblingImages.size());
  prevArrow = currentImageIndex > 0 ? Rect{side, arrowY, ARROW_W, ARROW_H} : Rect{};
  nextArrow = currentImageIndex >= 0 && currentImageIndex + 1 < count
                  ? Rect{width - side - ARROW_W, arrowY, ARROW_W, ARROW_H}
                  : Rect{};
  const int textH = renderer.getLineHeight(UI_12_FONT_ID);
  for (const auto& [rect, label] : {std::pair{prevArrow, "<"}, std::pair{nextArrow, ">"}}) {
    if (rect.width == 0) continue;
    renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 2, 6, true);
    const int textW = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::REGULAR);
    renderer.drawText(UI_12_FONT_ID, rect.x + (rect.width - textW) / 2, rect.y + (rect.height - textH) / 2, label, true,
                      EpdFontFamily::REGULAR);
  }
  if (currentImageIndex >= 0 && count > 0) {
    char position[24];
    snprintf(position, sizeof(position), "%d / %d", currentImageIndex + 1, count);
    renderer.drawCenteredText(UI_12_FONT_ID, arrowY + (ARROW_H - textH) / 2, position);
  }
  overlayShown = true;
}

// The bars in one gray plane. Absolute planes hold every pixel's level, so the
// bars are drawn as on the B/W layer; relative planes only mark pixels to gray.
void BmpViewerActivity::drawOverlayGrayPlane(const bool absolute) {
  const auto mode = renderer.getRenderMode();
  renderer.setRenderMode(GfxRenderer::BW);
  if (absolute) {
    drawOverlayContent();
  } else {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int width = renderer.getScreenWidth();
    const int footHeight = OVERLAY_ARROW_H + 2 * metrics.verticalSpacing;
    renderer.fillRect(0, 0, width, metrics.topPadding + metrics.headerHeight, true);
    renderer.fillRect(0, renderer.getScreenHeight() - footHeight, width, footHeight, true);
  }
  renderer.setRenderMode(mode);
}

void BmpViewerActivity::drawOverlay() {
  // A B/W refresh over a gray image turns its grays black, so gray screens are
  // shown again from the cache with the bars in every layer.
  if (!screenKey.empty()) {
    screencache::Reader cache;
    if (cache.open(screenKey, renderer) && cache.gray() && showCachedScreen(cache, true)) return;
  }
  drawOverlayContent();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  auto openSibling = [this](const int delta) {
    if (currentImageIndex < 0) {
      return false;
    }
    keepOverlay = overlayShown;
    const int nextIndex = currentImageIndex + delta;
    if (siblingImages.size() <= 1 || nextIndex < 0 || nextIndex >= static_cast<int>(siblingImages.size())) {
      return false;
    }
    currentImageIndex = nextIndex;
    std::string dirPath = FsHelpers::extractFolderPath(filePath);
    if (dirPath.back() != '/') dirPath += "/";
    filePath = dirPath + siblingImages[currentImageIndex];
    onEnter();
    keepOverlay = false;
    return true;
  };

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (returnToCaller) {
      finish();
    } else {
      activityManager.goToFileBrowser(filePath);
    }
    return;
  }

  // Like the stock Picco viewer: a tap shows the bars, a tap on an arrow
  // changes the image, any other tap hides them.
  int tapX = 0;
  int tapY = 0;
  if (BoardConfig::hasTouch() && mappedInput.wasScreenTapped(tapX, tapY)) {
    const auto inside = [tapX, tapY](const Rect& r) {
      return r.width > 0 && tapX >= r.x && tapX < r.x + r.width && tapY >= r.y && tapY < r.y + r.height;
    };
    if (overlayShown && inside(prevArrow)) {
      openSibling(-1);
    } else if (overlayShown && inside(nextArrow)) {
      openSibling(1);
    } else if (overlayShown) {
      onEnter();
    } else {
      drawOverlay();
    }
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    openSibling(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    openSibling(-1);
    return;
  }

  // Touch boards draw no button hints; long-press stands in for Confirm, as for file browser actions.
  int touchX = 0;
  int touchY = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenLongPress(touchX, touchY)) {
    if (!canSetSleepCover()) return;

    auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_SET_SLEEP_COVER_CONFIRM),
                                                                filePath.substr(filePath.find_last_of('/') + 1));
    if (!confirmation) {
      LOG_ERR("BMP", "OOM: sleep cover confirmation (%zu bytes)", sizeof(ConfirmationActivity));
      return;
    }
    startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
      if (result.isCancelled) {
        onEnter();
        return;
      }
      doSetSleepCover();
    });
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openSibling(1);
    return;
  }
}

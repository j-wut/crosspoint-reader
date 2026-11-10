#include "ImageScreen.h"

#include <Bitmap.h>
#include <BmpToBmpConverter.h>
#include <Epub/converters/JpegToFramebufferConverter.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

#include <algorithm>
#include <cmath>

#include "apps/ScreenCache.h"
#include "apps/Wallpaper.h"

namespace imagescreen {
namespace {
constexpr char SCALED_BMP[] = "/.crosspoint/view_scaled.bmp";
constexpr GfxRenderer::RenderMode GRAY_PLANES[] = {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB};

bool absoluteGray(const GfxRenderer& renderer) {
  return renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
}

// Draws gray planes in the encoding the viewer and sleep screen show them
// with, without a refresh; back to BW mode afterwards.
class OffscreenPlanes {
 public:
  OffscreenPlanes(GfxRenderer& renderer, const bool absolute) : renderer(renderer) {
    renderer.setGrayPlanesAbsolute(absolute);
  }
  ~OffscreenPlanes() {
    renderer.setGrayPlanesAbsolute(false);
    renderer.setRenderMode(GfxRenderer::BW);
  }
  OffscreenPlanes(const OffscreenPlanes&) = delete;
  OffscreenPlanes& operator=(const OffscreenPlanes&) = delete;

 private:
  GfxRenderer& renderer;
};

// The BMP draw path picks its thresholds by controller.
bool originalThresholds(const GfxRenderer& renderer) {
  return absoluteGray(renderer) && display.getController() == HalDisplay::Controller::SSD1677;
}

// Writes the image to SCALED_BMP as a 2-bit BMP: shrunk to fit width x height
// when larger (never enlarged), then dithered. PNGs are always converted; BMPs only
// when they need shrinking. False when no copy is needed or it fails.
bool writeScaledImage(const std::string& path, const int width, const int height, const bool thresholds) {
  const bool png = FsHelpers::hasPngExtension(path);
  HalFile in;
  if (!Storage.openFileForRead("BMP", path, in)) return false;
  int sourceWidth = 0;
  int sourceHeight = 0;
  if (png) {
    if (!PngToBmpConverter::pngFileSize(in, sourceWidth, sourceHeight)) return false;
  } else {
    Bitmap source(in);
    if (source.parseHeaders() != BmpReaderError::Ok) return false;
    sourceWidth = source.getWidth();
    sourceHeight = source.getHeight();
  }
  const bool shrink = sourceWidth > width || sourceHeight > height;
  if (!png && !shrink) return false;
  const int targetWidth = shrink ? width : sourceWidth;
  const int targetHeight = shrink ? height : sourceHeight;
  const unsigned long start = millis();
  HalFile out;
  if (!Storage.openFileForWrite("BMP", SCALED_BMP, out)) return false;
  bool ok = png ? PngToBmpConverter::pngFileToBmpStreamWithSize(in, out, targetWidth, targetHeight, false, thresholds)
                : BmpToBmpConverter::bmpFileToBmpStreamWithSize(in, out, targetWidth, targetHeight, false, thresholds);
  ok = out.close() && ok;
  if (!ok) Storage.remove(SCALED_BMP);
  LOG_INF("BMP", "converted %s %dx%d to fit %dx%d: %s, %lu ms", path.c_str(), sourceWidth, sourceHeight, targetWidth,
          targetHeight, ok ? "ok" : "failed", millis() - start);
  return ok;
}

// Removes the scaled copy once its layers are drawn.
struct ScaledBmpCleanup {
  bool active = false;
  ~ScaledBmpCleanup() {
    if (active) Storage.remove(SCALED_BMP);
  }
};

bool buildOpg(GfxRenderer& renderer, const std::string& path, screencache::Writer& capture) {
  renderer.clearScreen();
  if (!wallpaper::drawOpg(renderer, path).ok) return false;
  if (!capture.begin(renderer, false, false)) return false;
  capture.addLayer(renderer);
  return true;
}

bool buildJpeg(GfxRenderer& renderer, const std::string& path, screencache::Writer& capture) {
  JpegToFramebufferConverter decoder;
  ImageDimensions dimensions;
  if (!decoder.getDimensions(path, dimensions) || dimensions.width <= 0 || dimensions.height <= 0) return false;
  const RenderConfig config = fitJpeg(dimensions, renderer);
  const bool absolute = absoluteGray(renderer);
  if (!capture.begin(renderer, true, absolute)) return false;
  renderer.clearScreen();
  if (!decoder.decodeToFramebuffer(path, renderer, config)) return false;
  capture.addLayer(renderer);
  const OffscreenPlanes planes(renderer, absolute);
  for (const auto mode : GRAY_PLANES) {
    renderer.clearScreen(absolute ? 0xFF : 0x00);
    renderer.setRenderMode(mode);
    if (!decoder.decodeToFramebuffer(path, renderer, config)) return false;
    capture.addLayer(renderer);
  }
  return true;
}

bool buildBmp(GfxRenderer& renderer, const std::string& path, screencache::Writer& capture) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const bool thresholds = originalThresholds(renderer);
  // Shrink if needed, then dither; a BMP that fits is drawn as it is.
  ScaledBmpCleanup scaled;
  scaled.active = writeScaledImage(path, pageWidth, pageHeight, thresholds);
  if (!scaled.active && FsHelpers::hasPngExtension(path)) return false;

  HalFile file;
  if (!Storage.openFileForRead("BMP", scaled.active ? std::string(SCALED_BMP) : path, file)) return false;
  Bitmap bitmap(file, true, thresholds);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
  int x;
  int y;
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // Only when the shrunk copy could not be made: drawBitmap shrinks it, keeping the aspect ratio.
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
    if (ratio > screenRatio) {
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  renderer.clearScreen();
  if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) return false;
  const bool gray = bitmap.hasGreyscale();
  const bool absolute = absoluteGray(renderer);
  if (!capture.begin(renderer, gray, gray && absolute)) return false;
  capture.addLayer(renderer);
  if (!gray) return true;
  const OffscreenPlanes planes(renderer, absolute);
  for (const auto mode : GRAY_PLANES) {
    if (bitmap.rewindToData() != BmpReaderError::Ok) return false;
    renderer.clearScreen(absolute ? 0xFF : 0x00);
    renderer.setRenderMode(mode);
    if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) return false;
    capture.addLayer(renderer);
  }
  return true;
}
}  // namespace

std::string cacheKey(const std::string& path, const GfxRenderer& renderer) {
  const std::string file = screencache::fileKey("view", path);
  if (file.empty()) return {};
  // "fit4" marks screens shrunk (never enlarged) before dithering, replacing older ones.
  return file + "|fit4|" + std::to_string(renderer.getScreenWidth()) + 'x' +
         std::to_string(renderer.getScreenHeight()) + '|' +
         std::to_string(static_cast<int>(renderer.getOrientation())) + (absoluteGray(renderer) ? "|a" : "") +
         (display.getController() == HalDisplay::Controller::SSD1677 ? "|ssd1677" : "");
}

bool build(GfxRenderer& renderer, const std::string& path, const std::string& key) {
  if (key.empty()) return false;
  const unsigned long start = millis();
  screencache::Writer capture;
  capture.prepare(key);
  bool ok;
  if (wallpaper::isOpg(path)) {
    ok = buildOpg(renderer, path, capture);
  } else if (FsHelpers::hasJpgExtension(path)) {
    ok = buildJpeg(renderer, path, capture);
  } else {
    ok = buildBmp(renderer, path, capture);
  }
  ok = ok && capture.finish();
  LOG_INF("BMP", "screen for %s: %s, %lu ms", path.c_str(), ok ? "ok" : "failed", millis() - start);
  return ok;
}

bool hasScreen(const std::string& key, const GfxRenderer& renderer) {
  screencache::Reader cache;
  return cache.open(key, renderer);
}

bool buildBw(GfxRenderer& renderer, const std::string& viewerKey, const std::string& key, const bool invert) {
  screencache::Reader source;
  if (!source.open(viewerKey, renderer) || !source.readLayer(0, renderer)) return false;
  if (invert) renderer.invertScreen();
  screencache::Writer capture;
  if (!capture.begin(key, renderer, false, false)) return false;
  capture.addLayer(renderer);
  return capture.finish();
}

RenderConfig fitJpeg(const ImageDimensions& dimensions, const GfxRenderer& renderer) {
  const float scale = std::min({static_cast<float>(renderer.getScreenWidth()) / dimensions.width,
                                static_cast<float>(renderer.getScreenHeight()) / dimensions.height, 1.0f});
  const int width = std::max(1, std::min(renderer.getScreenWidth(), static_cast<int>(dimensions.width * scale)));
  const int height = std::max(1, std::min(renderer.getScreenHeight(), static_cast<int>(dimensions.height * scale)));
  RenderConfig config{(renderer.getScreenWidth() - width) / 2, (renderer.getScreenHeight() - height) / 2, width,
                      height};
  config.useExactDimensions = true;
  return config;
}

}  // namespace imagescreen

#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

class GfxRenderer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  float sourceCropX = 0.0f;         // Fraction cropped equally from the left and right edges
  float sourceCropY = 0.0f;         // Fraction cropped equally from the top and bottom edges
  bool preserveAlpha = false;       // Skip transparent pixels instead of compositing them against white
  std::string cachePath;            // If non-empty, decoder will write pixel cache to this path
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

  // Why the last decodeToFramebuffer call failed, shown in the reader's empty
  // image box. reason is null when the decoder gave none. code is the decoder
  // library's error number, valid when hasCode is set.
  struct Failure {
    const char* reason = nullptr;
    int code = 0;
    bool hasCode = false;
  };
  const Failure& lastFailure() const { return failure; }

  // Call from per-row/per-MCU decode callbacks (free functions, hence public):
  // yields one tick at most every 250 ms so multi-second decodes keep the idle
  // task (and its watchdog) fed. `lastYieldMs` is caller-held state,
  // initialized to the decode start time.
  static void yieldDuringDecode(uint32_t& lastYieldMs);

  // Validate decoder/header dimensions before narrowing them into the layout
  // representation. Shared by header probing and decoder fallbacks.
  static bool validateAndStoreDimensions(int64_t width, int64_t height, ImageDimensions& out, const char* format,
                                         int64_t maxPixels = MAX_SOURCE_PIXELS);

  // PNGs over MAX_SOURCE_PIXELS are decoded by the Gallery's PNG converter
  // (PngToBmpConverter), which streams rows and scales to the box. Its limit.
  static constexpr int64_t LARGE_PNG_MAX_DIMENSION = 8192;
  static constexpr int64_t LARGE_PNG_MAX_PIXELS = LARGE_PNG_MAX_DIMENSION * LARGE_PNG_MAX_DIMENSION;

 protected:
  // Size validation helpers. The cap bounds decode TIME, not memory: both decoders
  // stream (JPEG in MCU bands at 1/2..1/8 coarse scale, PNG scanline-by-scanline
  // with its own width-based row-buffer guard), so RAM never scales with source
  // area. 8 MP admits real-world ebook covers (KDP recommends 1600x2560 and
  // 2000x3000) while keeping a worst-case single decode in single-digit seconds;
  // the row callbacks yield periodically so a long decode cannot starve the idle
  // task's watchdog.
  static constexpr int64_t MAX_SOURCE_DIMENSION = INT16_MAX;
  static constexpr int64_t MAX_SOURCE_PIXELS = 8388608;  // 8 MP (e.g. 2048 * 4096)

  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);

  void clearFailure() { failure = Failure{}; }
  bool fail(const char* reason) {
    failure = Failure{reason, 0, false};
    return false;
  }
  bool fail(const char* reason, const int code) {
    failure = Failure{reason, code, true};
    return false;
  }

  Failure failure;
};

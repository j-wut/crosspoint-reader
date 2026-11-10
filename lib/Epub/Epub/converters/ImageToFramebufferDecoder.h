#pragma once
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
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

// Decoder objects are large (JPEGDEC ~17 KB, PNG ~42 KB). They go in the
// default heap when it has room. Boards with PSRAM use PSRAM when the internal
// heap is short, so a tight moment draws the image instead of an empty box.
template <typename T>
struct DecoderDeleter {
  void operator()(T* p) const {
    p->~T();
    heap_caps_free(p);
  }
};
template <typename T>
using DecoderPtr = std::unique_ptr<T, DecoderDeleter<T>>;

template <typename T>
DecoderPtr<T> allocateDecoder(const char* tag, const size_t minFreeHeap) {
  const size_t freeHeap = ESP.getFreeHeap();
  void* mem = freeHeap >= minFreeHeap ? malloc(sizeof(T)) : nullptr;
#if defined(BOARD_HAS_PSRAM)
  if (!mem) {
    mem = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem)
      LOG_INF(tag, "Decoder in PSRAM (heap %u free, wants %u)", static_cast<unsigned>(freeHeap),
              static_cast<unsigned>(minFreeHeap));
  }
#endif
  if (!mem) {
    LOG_ERR(tag, "Not enough memory for decoder (heap %u free, need %u)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(minFreeHeap));
    return nullptr;
  }
  return DecoderPtr<T>(new (mem) T());
}

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

  // Call from per-row/per-MCU decode callbacks (free functions, hence public):
  // yields one tick at most every 250 ms so multi-second decodes keep the idle
  // task (and its watchdog) fed. `lastYieldMs` is caller-held state,
  // initialized to the decode start time.
  static void yieldDuringDecode(uint32_t& lastYieldMs);

  // Validate decoder/header dimensions before narrowing them into the layout
  // representation. Shared by header probing and decoder fallbacks.
  static bool validateAndStoreDimensions(int64_t width, int64_t height, ImageDimensions& out, const char* format,
                                         int64_t maxPixels = MAX_SOURCE_PIXELS);

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
};

#pragma once

#include <HalStorage.h>

#include <string>

class GfxRenderer;

// A finished full-screen image kept as raw frame buffers: the B/W layer, then
// the LSB and MSB gray layers when the image has grays. Loading the layers
// skips decoding and drawing the source image again.
namespace screencache {

// Start of a cache key for one source file: its path, size, time and a sample
// of its bytes, plus the cache version. Callers append the screen size and
// every setting that changes the finished layers. Empty when the file cannot
// be read.
std::string fileKey(const char* kind, const std::string& path);

// The cache file for `key`, in /.crosspoint/screens.
std::string pathFor(const std::string& key);

// Writes the layers as they are drawn. A file is kept only after finish().
class Writer {
 public:
  ~Writer();
  bool begin(const std::string& key, const GfxRenderer& renderer, bool gray, bool absolute);
  // For callers that know the key before the layer count: prepare(), then begin() when drawing starts.
  void prepare(const std::string& key) { pendingKey = key; }
  bool begin(const GfxRenderer& renderer, bool gray, bool absolute) {
    return begin(pendingKey, renderer, gray, absolute);
  }
  // Saves the frame buffer as the next layer.
  void addLayer(const GfxRenderer& renderer);
  bool finish();

 private:
  std::string pendingKey;
  std::string path;
  std::string tmpPath;
  HalFile file;
  bool ok = false;
};

// Reads layers back into the frame buffer.
class Reader {
 public:
  // False when there is no usable cache for `key` on this screen.
  bool open(const std::string& key, const GfxRenderer& renderer);
  bool gray() const { return layers == 3; }
  bool absolute() const { return absoluteLayers; }
  // Reads layer 0 (B/W), 1 (LSB) or 2 (MSB) into the frame buffer.
  bool readLayer(int layer, const GfxRenderer& renderer);

 private:
  HalFile file;
  size_t dataStart = 0;
  uint8_t layers = 0;
  bool absoluteLayers = false;
};

}  // namespace screencache

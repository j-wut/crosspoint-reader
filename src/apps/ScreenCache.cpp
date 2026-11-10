#include "ScreenCache.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdint>
#include <cstdio>
#include <functional>

namespace screencache {
namespace {
constexpr uint32_t MAGIC = 0x31524353;  // "SCR1"

struct Header {
  uint32_t magic;
  uint32_t bufferSize;
  uint8_t layers;  // 1 = B/W, 3 = B/W + LSB + MSB
  uint8_t absolute;
  uint16_t keyLength;
};

constexpr char DIR[] = "/.crosspoint/screens";
// Bump when any drawing pipeline (decoders, dithering, gray layers) changes
// its output, so screens saved by older firmware are not shown.
constexpr char VERSION[] = "v1";
constexpr int SAMPLE_BLOCKS = 8;
constexpr size_t SAMPLE_SIZE = 512;
}  // namespace

std::string fileKey(const char* kind, const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("SCR", path, file)) return {};
  const size_t size = file.size();
  // Files the device writes all get the same date, so a replaced file of the
  // same size is told apart by sampled bytes (start, end and evenly between).
  uint64_t hash = 1469598103934665603ULL;  // FNV-1a
  uint8_t block[SAMPLE_SIZE];
  for (int i = 0; i < SAMPLE_BLOCKS; i++) {
    const size_t span = size > SAMPLE_SIZE ? size - SAMPLE_SIZE : 0;
    if (!file.seek(span * i / (SAMPLE_BLOCKS - 1))) return {};
    const int n = file.read(block, sizeof(block));
    if (n < 0) return {};
    for (int j = 0; j < n; j++) {
      hash ^= block[j];
      hash *= 1099511628211ULL;
    }
  }
  char sample[17];
  snprintf(sample, sizeof(sample), "%016llx", static_cast<unsigned long long>(hash));
  return std::string(kind) + '|' + VERSION + '|' + path + '|' + std::to_string(size) + '|' +
         std::to_string(file.modificationTime()) + '|' + sample;
}

std::string pathFor(const std::string& key) {
  return std::string(DIR) + "/scr_" + std::to_string(std::hash<std::string>{}(key)) + ".bin";
}

Writer::~Writer() {
  if (file.isOpen()) {
    file.close();
    Storage.remove(tmpPath.c_str());
  }
}

bool Writer::begin(const std::string& key, const GfxRenderer& renderer, const bool gray, const bool absolute) {
  if (key.empty() || key.size() > UINT16_MAX) return false;
  path = pathFor(key);
  tmpPath = path + ".tmp";
  if (!Storage.ensureDirectoryExists(DIR) || !Storage.openFileForWrite("SCR", tmpPath, file)) return false;
  const Header header{MAGIC, static_cast<uint32_t>(renderer.getBufferSize()), static_cast<uint8_t>(gray ? 3 : 1),
                      static_cast<uint8_t>(absolute ? 1 : 0), static_cast<uint16_t>(key.size())};
  ok = file.write(&header, sizeof(header)) == sizeof(header) && file.write(key.data(), key.size()) == key.size();
  return ok;
}

void Writer::addLayer(const GfxRenderer& renderer) {
  if (!file.isOpen() || !ok) return;
  ok = file.write(renderer.getFrameBuffer(), renderer.getBufferSize()) == renderer.getBufferSize();
}

bool Writer::finish() {
  if (!file.isOpen()) return false;
  bool saved = file.close() && ok;
  saved = saved && Storage.replaceFile(tmpPath.c_str(), path.c_str());
  if (!saved) Storage.remove(tmpPath.c_str());
  LOG_INF("SCR", "screen layers %s: %s", saved ? "saved" : "not saved", path.c_str());
  return saved;
}

bool Reader::open(const std::string& key, const GfxRenderer& renderer) {
  if (key.empty()) return false;
  const std::string path = pathFor(key);
  if (!Storage.exists(path.c_str()) || !Storage.openFileForRead("SCR", path, file)) return false;
  const size_t size = renderer.getBufferSize();
  Header header{};
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || header.magic != MAGIC ||
      header.bufferSize != size || header.keyLength != key.size() || (header.layers != 1 && header.layers != 3) ||
      file.size() != sizeof(header) + header.keyLength + static_cast<size_t>(header.layers) * size) {
    LOG_INF("SCR", "screen layers unusable: %s", path.c_str());
    file.close();
    return false;
  }
  std::string storedKey(header.keyLength, '\0');
  if (file.read(storedKey.data(), storedKey.size()) != static_cast<int>(storedKey.size()) || storedKey != key) {
    // Another image with the same hash; it is replaced on the next save.
    LOG_INF("SCR", "screen layers for another key: %s", path.c_str());
    file.close();
    return false;
  }
  dataStart = sizeof(header) + header.keyLength;
  layers = header.layers;
  absoluteLayers = header.absolute != 0;
  return true;
}

bool Reader::readLayer(const int layer, const GfxRenderer& renderer) {
  if (!file.isOpen() || layer < 0 || layer >= layers) return false;
  const size_t size = renderer.getBufferSize();
  return file.seek(dataStart + static_cast<size_t>(layer) * size) &&
         file.read(renderer.getFrameBuffer(), size) == static_cast<int>(size);
}

}  // namespace screencache

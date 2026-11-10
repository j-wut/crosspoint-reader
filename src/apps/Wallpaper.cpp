#include "Wallpaper.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <Print.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>

#include "CrossPointSettings.h"

namespace wallpaper {
namespace {

constexpr size_t COPY_BUFFER_SIZE = 2048;

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

bool readAt(HalFile& file, const uint32_t offset, void* out, const size_t len) {
  return file.seek(offset) && file.read(out, len) == static_cast<int>(len);
}

// FlatBuffer table field position: absolute offset of field `index` of the
// table at `table`, or 0 when the field is absent.
uint32_t fieldPos(HalFile& file, const uint32_t table, const int index, const uint32_t fileSize) {
  uint8_t buf[4];
  if (!readAt(file, table, buf, 4)) return 0;
  const auto vtable = static_cast<uint32_t>(static_cast<int64_t>(table) - static_cast<int32_t>(le32(buf)));
  if (vtable + 4 > fileSize) return 0;
  uint8_t head[4];
  if (!readAt(file, vtable, head, 4)) return 0;
  const uint16_t vtSize = head[0] | head[1] << 8;
  const uint32_t slot = 4 + 2 * static_cast<uint32_t>(index);
  if (slot + 2 > vtSize) return 0;
  uint8_t off[2];
  if (!readAt(file, vtable + slot, off, 2)) return 0;
  const uint16_t rel = off[0] | off[1] << 8;
  return rel == 0 ? 0 : table + rel;
}

bool fieldU32(HalFile& file, const uint32_t table, const int index, const uint32_t fileSize, uint32_t& out) {
  const uint32_t pos = fieldPos(file, table, index, fileSize);
  uint8_t buf[4];
  if (pos == 0 || pos + 4 > fileSize || !readAt(file, pos, buf, 4)) return false;
  out = le32(buf);
  return true;
}

void putLE16(uint8_t* p, const uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}

void putLE32(uint8_t* p, const uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF;
}

bool copyFile(const std::string& from, const char* to) {
  auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUFFER_SIZE);
  if (!buffer) return false;
  const std::string tmp = std::string(to) + ".tmp";
  HalFile in;
  HalFile out;
  if (!Storage.openFileForRead("WALL", from, in) || !Storage.openFileForWrite("WALL", tmp, out)) return false;
  bool ok = true;
  int n;
  while ((n = in.read(buffer.get(), COPY_BUFFER_SIZE)) > 0) {
    if (out.write(buffer.get(), static_cast<size_t>(n)) != static_cast<size_t>(n)) {
      ok = false;
      break;
    }
  }
  if (n < 0) ok = false;
  ok = out.close() && ok;
  ok = ok && Storage.replaceFile(tmp.c_str(), to);
  if (!ok) Storage.remove(tmp.c_str());
  return ok;
}

}  // namespace

bool isOpg(const std::string_view name) {
  return name.size() > 4 && strncasecmp(name.data() + name.size() - 4, ".opg", 4) == 0;
}

bool isImage(const std::string_view name) { return FsHelpers::hasImageExtension(name) || isOpg(name); }

std::vector<std::string> list() {
  std::vector<std::string> out;
  auto dir = Storage.open(DIR);
  if (!dir || !dir.isDirectory()) return out;
  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name, sizeof(name));
    if (name[0] == '.' || !isImage(name)) continue;
    out.push_back(std::string(DIR) + "/" + name);
  }
  FsHelpers::sortFileList(out);  // same order as the image viewer
  return out;
}

// .opg layout (one FlatBuffer, little endian, as the stock firmware and its
// built-in wallpapers use): the root table's field 10 is an (offset, size)
// pair pointing at the page list, whose entries are (offset, size) pairs of
// separately rooted page buffers. A page table holds f0 width, f1 height,
// f2 row bytes, f3 (pixel offset, size), f7 bits per channel (1 = 1, 2 = 2,
// 3 = 4) and f8 bits per pixel. When f8 is twice the channel bits, each pixel
// is gray then alpha (stock "gray-alpha"). Bits run most significant first,
// 0 = black / fully transparent.
struct OpgPage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint32_t data = 0;
  uint8_t channelBits = 0;
  uint8_t pixelBits = 0;
  bool hasAlpha() const { return pixelBits == 2 * channelBits; }
};

namespace {
bool parseOpg(HalFile& in, const std::string& pathForLog, OpgPage& page) {
  const auto fileSize = static_cast<uint32_t>(in.size());
  uint8_t buf[8];
  if (fileSize < 16 || !readAt(in, 0, buf, 4)) return false;
  const uint32_t root = le32(buf);
  const uint32_t pagesPos = fieldPos(in, root, 10, fileSize);
  if (pagesPos == 0 || !readAt(in, pagesPos, buf, 8)) return false;
  const uint32_t pagesOff = le32(buf);
  if (le32(buf + 4) < 8 || pagesOff + 8 > fileSize || !readAt(in, pagesOff, buf, 8)) return false;
  const uint32_t pageOff = le32(buf);
  if (pageOff + 4 > fileSize || !readAt(in, pageOff, buf, 4)) return false;
  const uint32_t table = pageOff + le32(buf);

  if (!fieldU32(in, table, 0, fileSize, page.width) || !fieldU32(in, table, 1, fileSize, page.height) ||
      !fieldU32(in, table, 2, fileSize, page.stride)) {
    return false;
  }
  const uint32_t dataPos = fieldPos(in, table, 3, fileSize);
  const uint32_t channelPos = fieldPos(in, table, 7, fileSize);
  const uint32_t bppPos = fieldPos(in, table, 8, fileSize);
  uint8_t code = 1;
  if (dataPos == 0 || bppPos == 0 || !readAt(in, dataPos, buf, 4) || !readAt(in, bppPos, &page.pixelBits, 1) ||
      (channelPos != 0 && !readAt(in, channelPos, &code, 1))) {
    return false;
  }
  page.data = le32(buf);
  page.channelBits = code == 3 ? 4 : code;
  const bool bitsOk = (page.channelBits == 1 || page.channelBits == 2 || page.channelBits == 4) &&
                      (page.pixelBits == page.channelBits || page.hasAlpha());
  if (!bitsOk || page.width == 0 || page.height == 0 || page.width > 4096 || page.height > 4096 ||
      page.stride < (page.width * page.pixelBits + 7) / 8 ||
      page.data + static_cast<uint64_t>(page.stride) * page.height > fileSize) {
    LOG_ERR("WALL", "Unsupported opg %s: %ux%u channel bits %u pixel bits %u", pathForLog.c_str(), page.width,
            page.height, page.channelBits, page.pixelBits);
    return false;
  }
  return true;
}

uint8_t readBits(const uint8_t* row, const uint32_t bit, const uint8_t count) {
  uint8_t value = 0;
  for (uint8_t i = 0; i < count; ++i) {
    const uint32_t b = bit + i;
    value = static_cast<uint8_t>(value << 1 | ((row[b >> 3] >> (7 - (b & 7))) & 1));
  }
  return value;
}
}  // namespace

// Writes the gray channel as a 1-, 2- or 4-bit BMP (the viewer and the old
// sleep modes read BMP). Transparent pixels become white.
bool opgToBmp(const std::string& opgPath, const std::string& bmpPath) {
  HalFile in;
  if (!Storage.openFileForRead("WALL", opgPath, in)) return false;
  OpgPage page;
  if (!parseOpg(in, opgPath, page)) return false;
  const uint32_t width = page.width;
  const uint32_t height = page.height;
  const uint8_t bpp = page.channelBits;

  const uint32_t rowBytes = (width * bpp + 31) / 32 * 4;
  const uint32_t colors = 1u << bpp;
  const uint32_t headerSize = 14 + 40 + 4 * colors;
  uint8_t header[14 + 40 + 64] = {};
  header[0] = 'B';
  header[1] = 'M';
  putLE32(header + 2, headerSize + rowBytes * height);
  putLE32(header + 10, headerSize);
  putLE32(header + 14, 40);
  putLE32(header + 18, width);
  putLE32(header + 22, height);  // bottom-up rows
  putLE16(header + 26, 1);
  putLE16(header + 28, bpp);
  putLE32(header + 34, rowBytes * height);
  putLE32(header + 46, colors);
  for (uint32_t i = 0; i < colors; ++i) {
    const auto level = static_cast<uint8_t>(i * 255 / (colors - 1));
    uint8_t* entry = header + 54 + 4 * i;
    entry[0] = entry[1] = entry[2] = level;
  }

  auto src = makeUniqueNoThrow<uint8_t[]>(page.stride);
  auto row = makeUniqueNoThrow<uint8_t[]>(rowBytes);
  if (!src || !row) return false;
  const std::string tmp = bmpPath + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite("WALL", tmp, out)) return false;
  bool ok = out.write(header, headerSize) == headerSize;
  const uint8_t maxLevel = static_cast<uint8_t>(colors - 1);
  for (uint32_t y = height; ok && y-- > 0;) {
    ok = readAt(in, page.data + y * page.stride, src.get(), page.stride);
    if (!ok) break;
    memset(row.get(), 0, rowBytes);
    for (uint32_t x = 0; x < width; ++x) {
      uint8_t gray = readBits(src.get(), x * page.pixelBits, bpp);
      if (page.hasAlpha() && readBits(src.get(), x * page.pixelBits + bpp, bpp) * 2 <= maxLevel) gray = maxLevel;
      const uint32_t bit = x * bpp;
      row[bit >> 3] |= static_cast<uint8_t>(gray << (8 - bpp - (bit & 7)));
    }
    ok = out.write(row.get(), rowBytes) == rowBytes;
  }
  ok = out.close() && ok;
  ok = ok && Storage.replaceFile(tmp.c_str(), bmpPath.c_str());
  if (!ok) Storage.remove(tmp.c_str());
  return ok;
}

namespace {
// PNG to a 2-bit BMP the way the full-screen viewer converts it: fitted images
// are only shrunk, never enlarged, and dithered once after scaling.
bool pngToScreenBmp(HalFile& src, Print& out, const bool crop, const bool thresholds) {
  // Portrait screen size, as for book covers.
  const int screenW = display.getDisplayHeight();
  const int screenH = display.getDisplayWidth();
  int width = 0;
  int height = 0;
  if (crop || !PngToBmpConverter::pngFileSize(src, width, height) || width > screenW || height > screenH) {
    return PngToBmpConverter::pngFileToBmpStream(src, out, crop, thresholds);
  }
  return PngToBmpConverter::pngFileToBmpStreamWithSize(src, out, width, height, false, thresholds);
}
}  // namespace

bool screenBmp(const std::string& path, std::string& bmpPath) {
  if (FsHelpers::hasBmpExtension(path)) {
    bmpPath = path;
    return Storage.exists(path.c_str());
  }
  HalFile src;
  if (!Storage.openFileForRead("WALL", path, src)) return false;
  const bool crop = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
  // Same-sized files share a byte count, so the key also needs the modification time.
  // Same thresholds as the full-screen viewer, so the sleep screen matches it.
  const bool thresholds = display.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported() &&
                          display.getController() == HalDisplay::Controller::SSD1677;
  const std::string key = path + '|' + std::to_string(src.size()) + '|' + std::to_string(src.modificationTime()) +
                          (crop ? "|c" : "|f") + (thresholds ? "|t" : "") + "|v3";
  bmpPath = "/.crosspoint/wp_" + std::to_string(std::hash<std::string>{}(key)) + ".bmp";
  if (Storage.exists(bmpPath.c_str())) return true;

  bool ok;
  if (isOpg(path)) {
    src.close();
    ok = opgToBmp(path, bmpPath);
  } else {
    const std::string tmp = bmpPath + ".tmp";
    HalFile out;
    ok = Storage.openFileForWrite("WALL", tmp, out) &&
         (FsHelpers::hasPngExtension(path) ? pngToScreenBmp(src, out, crop, thresholds)
                                           : JpegToBmpConverter::jpegFileToBmpStream(src, out, crop));
    if (out.isOpen()) ok = out.close() && ok;
    ok = ok && Storage.replaceFile(tmp.c_str(), bmpPath.c_str());
    if (!ok) Storage.remove(tmp.c_str());
  }
  if (!ok) LOG_ERR("WALL", "Cannot convert %s", path.c_str());
  return ok;
}

namespace {
constexpr uint32_t THUMB_HEADER = 14 + 40 + 8;  // 1-bit BMP with a 2-entry palette

// Collects a converter's BMP output in memory.
class MemoryBmp final : public Print {
 public:
  size_t write(const uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, const size_t len) override {
    if (failed || !reserve(size + len)) {
      failed = true;
      return 0;
    }
    memcpy(data.get() + size, buf, len);
    size += len;
    return len;
  }
  std::unique_ptr<uint8_t[]> data;
  size_t size = 0;
  bool failed = false;

 private:
  bool reserve(const size_t need) {
    if (need <= capacity) return true;
    const size_t next = std::max<size_t>(need, capacity == 0 ? 4096 : capacity * 2);
    auto grown = makeUniqueNoThrow<uint8_t[]>(next);
    if (!grown) return false;
    if (size > 0) memcpy(grown.get(), data.get(), size);
    data = std::move(grown);
    capacity = next;
    return true;
  }
  size_t capacity = 0;
};

struct ThumbLayout {
  int width = 0;
  int height = 0;
  bool topDown = false;
  bool zeroIsBlack = true;
  uint32_t rowBytes = 0;
  uint32_t dataOffset = 0;
};

// Checks that the buffer is a whole 1-bit BMP and reads its layout.
bool thumbLayout(const Thumb& thumb, ThumbLayout& layout) {
  const uint8_t* p = thumb.data.get();
  if (!p || thumb.size < THUMB_HEADER || p[0] != 'B' || p[1] != 'M' || (p[28] | p[29] << 8) != 1) return false;
  layout.dataOffset = le32(p + 10);
  layout.width = static_cast<int32_t>(le32(p + 18));
  const auto height = static_cast<int32_t>(le32(p + 22));
  layout.topDown = height < 0;
  layout.height = height < 0 ? -height : height;
  if (layout.width <= 0 || layout.height <= 0 || layout.width > 4096 || layout.height > 4096) return false;
  layout.rowBytes = (static_cast<uint32_t>(layout.width) + 31) / 32 * 4;
  const uint32_t paletteAt = 14 + le32(p + 14);
  if (paletteAt + 8 > thumb.size) return false;
  const auto luma = [p](const uint32_t at) { return p[at] + p[at + 1] + p[at + 2]; };
  layout.zeroIsBlack = luma(paletteAt) <= luma(paletteAt + 4);
  return static_cast<uint64_t>(layout.dataOffset) + static_cast<uint64_t>(layout.rowBytes) * layout.height <=
         thumb.size;
}

// One source row as 8-bit gray (0 = black), srcW values.
using RowFn = std::function<bool(int sy, uint8_t* gray)>;

// Box-filters the source down to fit maxWidth x maxHeight, then dithers it to
// 1 bit (a 4x4 ordered pattern) into a top-down BMP in memory. Rows are read
// in file order (bottom first for bottom-up files) so the card only seeks
// forward.
bool boxThumb(const int srcW, const int srcH, const bool bottomFirst, const int maxWidth, const int maxHeight,
              const RowFn& readRow, Thumb& out) {
  const float scale = std::min({1.0f, static_cast<float>(maxWidth) / srcW, static_cast<float>(maxHeight) / srcH});
  const int dstW = std::max(1, static_cast<int>(srcW * scale));
  const int dstH = std::max(1, static_cast<int>(srcH * scale));
  const uint32_t outRowBytes = (static_cast<uint32_t>(dstW) + 31) / 32 * 4;
  const size_t size = THUMB_HEADER + static_cast<size_t>(outRowBytes) * dstH;

  auto data = makeUniqueNoThrow<uint8_t[]>(size);
  auto gray = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(srcW));
  auto sums = makeUniqueNoThrow<uint32_t[]>(static_cast<size_t>(dstW));
  auto counts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(dstW));
  if (!data || !gray || !sums || !counts) {
    LOG_ERR("WALL", "OOM: thumb %dx%d", dstW, dstH);
    return false;
  }
  memset(data.get(), 0, size);
  uint8_t* header = data.get();
  header[0] = 'B';
  header[1] = 'M';
  putLE32(header + 2, size);
  putLE32(header + 10, THUMB_HEADER);
  putLE32(header + 14, 40);
  putLE32(header + 18, dstW);
  putLE32(header + 22, static_cast<uint32_t>(-dstH));  // top-down rows
  putLE16(header + 26, 1);
  putLE16(header + 28, 1);
  putLE32(header + 34, outRowBytes * dstH);
  putLE32(header + 46, 2);
  header[58] = header[59] = header[60] = 0xFF;  // palette: 0 black, 1 white

  memset(counts.get(), 0, sizeof(uint16_t) * dstW);
  for (int x = 0; x < srcW; ++x) ++counts[static_cast<int64_t>(x) * dstW / srcW];

  static constexpr uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  int dy = -1;
  uint32_t rows = 0;
  const auto flush = [&] {
    if (dy < 0 || rows == 0) return;
    uint8_t* row = data.get() + THUMB_HEADER + static_cast<size_t>(dy) * outRowBytes;
    for (int x = 0; x < dstW; ++x) {
      const uint32_t level = sums[x] / (static_cast<uint32_t>(counts[x]) * rows);
      if (level * 16 > BAYER[dy & 3][x & 3] * 255u + 127u) row[x >> 3] |= static_cast<uint8_t>(0x80 >> (x & 7));
    }
  };
  for (int i = 0; i < srcH; ++i) {
    const int sy = bottomFirst ? srcH - 1 - i : i;
    const int rowDy = static_cast<int>(static_cast<int64_t>(sy) * dstH / srcH);
    if (rowDy != dy) {
      flush();
      memset(sums.get(), 0, sizeof(uint32_t) * dstW);
      rows = 0;
      dy = rowDy;
    }
    if (!readRow(sy, gray.get())) return false;
    for (int x = 0; x < srcW; ++x) sums[static_cast<int64_t>(x) * dstW / srcW] += gray[x];
    ++rows;
  }
  flush();
  out.data = std::move(data);
  out.size = size;
  return true;
}
}  // namespace

std::string thumbCachePath(const std::string& path, const int maxWidth, const int maxHeight) {
  HalFile src;
  if (!Storage.openFileForRead("WALL", path, src)) return {};
  const bool crop = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
  // Same-sized files share a byte count, so the key also needs the modification time.
  const std::string key = path + '|' + std::to_string(src.size()) + '|' + std::to_string(src.modificationTime()) + '|' +
                          std::to_string(maxWidth) + 'x' + std::to_string(maxHeight) + (crop ? "|c" : "|f") + "|t2";
  return "/.crosspoint/wpt_" + std::to_string(std::hash<std::string>{}(key)) + ".bmp";
}

bool loadThumb(const std::string& cachePath, Thumb& out) {
  if (cachePath.empty() || !Storage.exists(cachePath.c_str())) return false;
  HalFile in;
  if (!Storage.openFileForRead("WALL", cachePath, in)) return false;
  const size_t size = in.size();
  Thumb thumb;
  thumb.data = makeUniqueNoThrow<uint8_t[]>(size);
  if (!thumb.data || in.read(thumb.data.get(), size) != static_cast<int>(size)) return false;
  thumb.size = size;
  ThumbLayout layout;
  if (!thumbLayout(thumb, layout)) {
    LOG_ERR("WALL", "bad cached preview %s", cachePath.c_str());
    return false;
  }
  out = std::move(thumb);
  return true;
}

bool makeThumb(const std::string& path, const int maxWidth, const int maxHeight, Thumb& out) {
  if (maxWidth <= 0 || maxHeight <= 0) return false;
  HalFile in;
  if (!Storage.openFileForRead("WALL", path, in)) return false;

  if (FsHelpers::hasPngExtension(path) || FsHelpers::hasJpgExtension(path)) {
    const bool crop = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
    MemoryBmp bmp;
    const bool ok = FsHelpers::hasPngExtension(path)
                        ? PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(in, bmp, maxWidth, maxHeight, crop)
                        : JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(in, bmp, maxWidth, maxHeight, crop);
    Thumb thumb;
    thumb.data = std::move(bmp.data);
    thumb.size = bmp.size;
    ThumbLayout layout;
    if (!ok || bmp.failed || !thumbLayout(thumb, layout)) return false;
    out = std::move(thumb);
    return true;
  }

  if (isOpg(path)) {
    OpgPage page;
    if (!parseOpg(in, path, page)) return false;
    // Rows are contiguous, so they are read in blocks of about 4 KB.
    const uint32_t blockRows = std::max<uint32_t>(1, 4096 / page.stride);
    auto block = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(blockRows) * page.stride);
    if (!block || !in.seek(page.data)) return false;
    const uint8_t maxLevel = static_cast<uint8_t>((1u << page.channelBits) - 1);
    uint32_t loaded = 0;
    uint32_t next = 0;
    return boxThumb(
        static_cast<int>(page.width), static_cast<int>(page.height), false, maxWidth, maxHeight,
        [&](int, uint8_t* gray) {
          if (next == loaded) {
            const int n = in.read(block.get(), static_cast<size_t>(blockRows) * page.stride);
            if (n < static_cast<int>(page.stride)) return false;
            loaded = static_cast<uint32_t>(n) / page.stride;
            next = 0;
          }
          const uint8_t* src = block.get() + static_cast<size_t>(next++) * page.stride;
          for (uint32_t x = 0; x < page.width; ++x) {
            const uint32_t bit = x * page.pixelBits;
            uint8_t level = readBits(src, bit, page.channelBits);
            if (page.hasAlpha() && readBits(src, bit + page.channelBits, page.channelBits) * 2 <= maxLevel) {
              level = maxLevel;  // transparent shows as white
            }
            gray[x] = static_cast<uint8_t>(level * 255 / maxLevel);
          }
          return true;
        },
        out);
  }

  Bitmap bitmap(in);
  const BmpReaderError err = bitmap.parseHeaders();
  if (err != BmpReaderError::Ok) {
    LOG_ERR("WALL", "thumb: bad BMP %s: %s", path.c_str(), Bitmap::errorToString(err));
    return false;
  }
  auto raw = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(bitmap.getRowBytes()));
  if (!raw) return false;
  return boxThumb(
      bitmap.getWidth(), bitmap.getHeight(), !bitmap.isTopDown(), maxWidth, maxHeight,
      [&](const int sy, uint8_t* gray) { return bitmap.readRowGray(sy, gray, raw.get()) == BmpReaderError::Ok; }, out);
}

bool saveThumb(const Thumb& thumb, const std::string& cachePath) {
  if (!thumb || cachePath.empty()) return false;
  const std::string tmp = cachePath + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite("WALL", tmp, out)) return false;
  bool ok = out.write(thumb.data.get(), thumb.size) == thumb.size;
  ok = out.close() && ok;
  ok = ok && Storage.replaceFile(tmp.c_str(), cachePath.c_str());
  if (!ok) Storage.remove(tmp.c_str());
  return ok;
}

void drawThumb(GfxRenderer& renderer, const Thumb& thumb, const int x, const int y, const int width, const int height) {
  ThumbLayout layout;
  if (!thumbLayout(thumb, layout)) return;
  // Centred; a cropped preview larger than the box is cut at its edges.
  const int left = x + (width - layout.width) / 2;
  const int top = y + (height - layout.height) / 2;
  const int x0 = std::max(0, x - left);
  const int x1 = std::min(layout.width, x + width - left);
  const int y0 = std::max(0, y - top);
  const int y1 = std::min(layout.height, y + height - top);
  for (int row = y0; row < y1; ++row) {
    const int fileRow = layout.topDown ? row : layout.height - 1 - row;
    const uint8_t* bits = thumb.data.get() + layout.dataOffset + static_cast<size_t>(fileRow) * layout.rowBytes;
    for (int col = x0; col < x1; ++col) {
      const bool one = bits[col >> 3] & (0x80 >> (col & 7));
      if (one != layout.zeroIsBlack) renderer.drawPixel(left + col, top + row, true);
    }
  }
}

bool setAs(const std::string& path, const char* destination) {
  std::string bmp;
  if (!screenBmp(path, bmp)) return false;
  return bmp == destination || copyFile(bmp, destination);
}

namespace {
char* storedField(const Kind kind) {
  switch (kind) {
    case READING:
      return SETTINGS.readerSleepWallpaperPath;
    case POWER_OFF:
      return SETTINGS.powerOffWallpaperPath;
    default:
      return SETTINGS.sleepWallpaperPath;
  }
}

uint8_t& alphaField(const Kind kind) {
  switch (kind) {
    case READING:
      return SETTINGS.readerSleepWallpaperAlpha;
    case POWER_OFF:
      return SETTINGS.powerOffWallpaperAlpha;
    default:
      return SETTINGS.sleepWallpaperAlpha;
  }
}
}  // namespace

const char* storedPath(const Kind kind) { return storedField(kind); }

bool keepsAlpha(const Kind kind) { return alphaField(kind) != 0; }

bool opgHasAlpha(const std::string& path) {
  if (!isOpg(path)) return false;
  HalFile in;
  if (!Storage.openFileForRead("WALL", path, in)) return false;
  OpgPage page;
  return parseOpg(in, path, page) && page.hasAlpha();
}

bool setWallpaper(const std::string& path, const Kind kind, const bool keepAlpha) {
  char* field = storedField(kind);
  uint8_t& alpha = alphaField(kind);
  const size_t size = sizeof(SETTINGS.sleepWallpaperPath);
  if (path.size() >= size) {
    LOG_ERR("WALL", "wallpaper path too long (%u): %s", static_cast<unsigned>(path.size()), path.c_str());
    return false;
  }
  if (path == field && alpha == keepAlpha) return true;
  strncpy(field, path.c_str(), size - 1);
  field[size - 1] = '\0';
  alpha = keepAlpha ? 1 : 0;
  const bool ok = SETTINGS.saveToFile();
  LOG_INF("WALL", "wallpaper set kind=%u path='%s' keepAlpha=%d saved=%d", kind, path.c_str(), keepAlpha, ok);
  return ok;
}

DrawResult drawOpg(GfxRenderer& renderer, const std::string& path, const bool keepAlpha) {
  DrawResult result;
  HalFile in;
  if (!Storage.openFileForRead("WALL", path, in)) {
    LOG_ERR("WALL", "wallpaper open failed: %s", path.c_str());
    return result;
  }
  OpgPage page;
  if (!parseOpg(in, path, page)) return result;
  auto src = makeUniqueNoThrow<uint8_t[]>(page.stride);
  if (!src) {
    LOG_ERR("WALL", "OOM: opg row %u", page.stride);
    return result;
  }

  // Stock sizing: shrink to fit (keeping the aspect ratio) only when larger
  // than the screen, round the width down to a multiple of 8, centre.
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  int dstW = static_cast<int>(page.width);
  int dstH = static_cast<int>(page.height);
  if (dstW > screenW || dstH > screenH) {
    const float scale = std::min(static_cast<float>(screenW) / page.width, static_cast<float>(screenH) / page.height);
    dstW = std::max(1, static_cast<int>(page.width * scale));
    dstH = std::max(1, static_cast<int>(page.height * scale));
    if (dstW >= 8) dstW = dstW / 8 * 8;
  }
  const int x0 = (screenW - dstW) / 2;
  const int y0 = (screenH - dstH) / 2;
  const bool fileAlpha = page.hasAlpha();
  result.hasAlpha = fileAlpha && keepAlpha;
  LOG_INF("WALL", "drawing opg %s %ux%u -> %dx%d at %d,%d ga=%d keep=%d channel bits %u pixel bits %u", path.c_str(),
          page.width, page.height, dstW, dstH, x0, y0, fileAlpha, keepAlpha, page.channelBits, page.pixelBits);

  if (!result.hasAlpha) renderer.clearScreen(0xFF);
  const uint8_t maxLevel = static_cast<uint8_t>((1u << page.channelBits) - 1);
  int loadedRow = -1;
  for (int y = 0; y < dstH; ++y) {
    const int sy = static_cast<int>(static_cast<int64_t>(y) * page.height / dstH);
    if (sy != loadedRow) {
      if (!readAt(in, page.data + static_cast<uint32_t>(sy) * page.stride, src.get(), page.stride)) {
        LOG_ERR("WALL", "opg read failed row %d: %s", sy, path.c_str());
        return result;
      }
      loadedRow = sy;
    }
    for (int x = 0; x < dstW; ++x) {
      const uint32_t sx = static_cast<uint32_t>(static_cast<int64_t>(x) * page.width / dstW);
      const uint32_t bit = sx * page.pixelBits;
      // Transparent: keep the screen, or the white cleared above.
      if (fileAlpha && readBits(src.get(), bit + page.channelBits, page.channelBits) * 2 <= maxLevel) continue;
      const bool black = readBits(src.get(), bit, page.channelBits) * 2 <= maxLevel;
      renderer.drawPixel(x0 + x, y0 + y, black);
    }
  }
  result.ok = true;
  return result;
}

}  // namespace wallpaper

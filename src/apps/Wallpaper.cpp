#include "Wallpaper.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>

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
  std::sort(out.begin(), out.end());
  return out;
}

// .opg layout (one FlatBuffer, little endian, as the stock firmware and its
// built-in wallpapers use): the root table's field 10 is an (offset, size)
// pair pointing at the page list, whose entries are (offset, size) pairs of
// separately rooted page buffers. A page table holds f0 width, f1 height,
// f2 row bytes, f3 (pixel offset, size) and f8 bits per pixel (1 or 2,
// most significant bits first, 0 = black).
bool opgToBmp(const std::string& opgPath, const std::string& bmpPath) {
  HalFile in;
  if (!Storage.openFileForRead("WALL", opgPath, in)) return false;
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
  const uint32_t page = pageOff + le32(buf);

  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  if (!fieldU32(in, page, 0, fileSize, width) || !fieldU32(in, page, 1, fileSize, height) ||
      !fieldU32(in, page, 2, fileSize, stride)) {
    return false;
  }
  const uint32_t dataPos = fieldPos(in, page, 3, fileSize);
  const uint32_t bppPos = fieldPos(in, page, 8, fileSize);
  uint8_t bpp = 0;
  if (dataPos == 0 || bppPos == 0 || !readAt(in, dataPos, buf, 4) || !readAt(in, bppPos, &bpp, 1)) return false;
  const uint32_t data = le32(buf);
  if ((bpp != 1 && bpp != 2) || width == 0 || height == 0 || width > 4096 || height > 4096 ||
      stride < (width * bpp + 7) / 8 || data + static_cast<uint64_t>(stride) * height > fileSize) {
    LOG_ERR("WALL", "Unsupported opg %s: %ux%u bpp %u", opgPath.c_str(), width, height, bpp);
    return false;
  }

  const uint32_t rowBytes = (width * bpp + 31) / 32 * 4;
  const uint32_t colors = 1u << bpp;
  const uint32_t headerSize = 14 + 40 + 4 * colors;
  uint8_t header[14 + 40 + 16] = {};
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

  auto row = makeUniqueNoThrow<uint8_t[]>(rowBytes);
  if (!row) return false;
  memset(row.get(), 0, rowBytes);
  const std::string tmp = bmpPath + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite("WALL", tmp, out)) return false;
  bool ok = out.write(header, headerSize) == headerSize;
  const uint32_t copyBytes = std::min(stride, rowBytes);
  for (uint32_t y = height; ok && y-- > 0;) {
    ok = readAt(in, data + y * stride, row.get(), copyBytes) && out.write(row.get(), rowBytes) == rowBytes;
  }
  ok = out.close() && ok;
  ok = ok && Storage.replaceFile(tmp.c_str(), bmpPath.c_str());
  if (!ok) Storage.remove(tmp.c_str());
  return ok;
}

bool screenBmp(const std::string& path, std::string& bmpPath) {
  if (FsHelpers::hasBmpExtension(path)) {
    bmpPath = path;
    return Storage.exists(path.c_str());
  }
  HalFile src;
  if (!Storage.openFileForRead("WALL", path, src)) return false;
  const bool crop = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
  // Same-sized files share a byte count, so the key also needs the modification time.
  const std::string key =
      path + '|' + std::to_string(src.size()) + '|' + std::to_string(src.modificationTime()) + (crop ? "|c" : "|f");
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
         (FsHelpers::hasPngExtension(path) ? PngToBmpConverter::pngFileToBmpStream(src, out, crop)
                                           : JpegToBmpConverter::jpegFileToBmpStream(src, out, crop));
    if (out.isOpen()) ok = out.close() && ok;
    ok = ok && Storage.replaceFile(tmp.c_str(), bmpPath.c_str());
    if (!ok) Storage.remove(tmp.c_str());
  }
  if (!ok) LOG_ERR("WALL", "Cannot convert %s", path.c_str());
  return ok;
}

bool setAs(const std::string& path, const char* destination) {
  std::string bmp;
  if (!screenBmp(path, bmp)) return false;
  return bmp == destination || copyFile(bmp, destination);
}

}  // namespace wallpaper

#include "BookProgress.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>

#include "MirroredPath.h"

namespace {
uint32_t readLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
}  // namespace

int loadBookProgress(const std::string& path) { return loadBookProgressInfo(path, false).percent; }

BookProgressInfo loadBookProgressInfo(const std::string& path, const bool withChapter) {
  BookProgressInfo info;
  uint8_t data[10]{};
  if (FsHelpers::hasReflowableBookExtension(path)) {
    // Metadata objects exceed the stack budget; only the featured book is loaded, once per entry.
    auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: progress metadata");
      return info;
    }
    if (!epub->load(false, true)) return info;
    MirroredPath::adoptLegacyProgress(path, epub->getCachePath());
    HalFile file;
    if (!Storage.openFileForRead("HOME", MirroredPath::progressFor(path), file)) return info;
    const int size = file.read(data, sizeof(data));
    if (size != 4 && size != 6 && size != 10) return info;
    const int spine = data[0] | (data[1] << 8);
    const int page = data[2] | (data[3] << 8);
    const int total = size >= 6 ? data[4] | (data[5] << 8) : 0;
    if (epub->getSpineItemsCount() <= 0 || epub->getBookSize() == 0) return info;
    if (spine == epub->getSpineItemsCount()) {
      info.percent = 100;
      return info;
    }
    if (spine > epub->getSpineItemsCount()) return info;
    const bool pageKnown = total > 0 && page != UINT16_MAX;
    const float fraction = pageKnown ? std::clamp(float(page) / total, 0.0f, 1.0f) : 0;
    info.percent = std::clamp(static_cast<int>(epub->calculateProgress(spine, fraction) * 100 + 0.5f), 0, 100);
    // The chapter title costs one more SD read, so list views skip it.
    const int tocIndex = withChapter ? epub->getTocIndexForSpineIndex(spine) : -1;
    if (tocIndex >= 0) info.chapter = epub->getTocItem(tocIndex).title;
    if (pageKnown) {
      info.page = std::min(page + 1, total);
      info.chapterPages = total;
    }
    return info;
  }
  if (FsHelpers::hasXtcExtension(path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: XTC progress metadata");
      return info;
    }
    if (!xtc->load()) return info;
    MirroredPath::adoptLegacyProgress(path, xtc->getCachePath());
    HalFile file;
    if (!Storage.openFileForRead("HOME", MirroredPath::progressFor(path), file) || file.read(data, 4) != 4) return info;
    const uint32_t page = readLe32(data);
    if (xtc->getPageCount() == 0) return info;
    info.percent = page >= xtc->getPageCount() ? 100 : xtc->calculateProgress(page);
  }
  return info;
}

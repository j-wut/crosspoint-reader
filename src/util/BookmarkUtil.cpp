#include "BookmarkUtil.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <functional>
#include <string>

#include "MirroredPath.h"

std::string BookmarkUtil::getBookmarksDir() { return "/.crosspoint/bookmarks/"; }

namespace {
// Old naming: a.txt and a.epub, or /x/y.epub and /x_y.epub, shared one file.
std::string legacyBookmarkPath(const std::string& bookPath) {
  std::string bookName = std::string(bookPath).erase(0, 1);
  std::replace(bookName.begin(), bookName.end(), '/', '_');
  std::replace(bookName.begin(), bookName.end(), '\\', '_');
  const size_t lastDot = bookName.find_last_of('.');
  if (lastDot != std::string::npos) {
    bookName.erase(lastDot);
  }
  return BookmarkUtil::getBookmarksDir() + bookName + ".json";
}
}  // namespace

std::string BookmarkUtil::getBookmarkPath(const std::string& bookPath) {
  return MirroredPath::forBook(MirroredPath::BOOKMARKS_ROOT, bookPath, ".json");
}

void BookmarkUtil::adoptLegacyBookmarks(const std::string& bookPath) {
  const std::string path = getBookmarkPath(bookPath);
  if (Storage.exists(path.c_str())) return;
  const std::string legacy = legacyBookmarkPath(bookPath);
  if (!Storage.exists(legacy.c_str())) return;
  MirroredPath::ensureParent(path);
  if (!Storage.rename(legacy.c_str(), path.c_str())) {
    LOG_ERR("BKM", "Failed to adopt legacy bookmarks: %s", legacy.c_str());
  }
}

std::string BookmarkUtil::sanitizeBookmarkSummary(std::string summary) {
  summary.erase(
      std::unique(summary.begin(), summary.end(), [](char a, char b) { return std::isspace(a) && std::isspace(b); }),
      summary.end());
  summary.erase(std::remove(summary.begin(), summary.end(), '\n'), summary.end());
  summary.erase(summary.begin(),
                std::find_if(summary.begin(), summary.end(), [](unsigned char ch) { return !std::isspace(ch); }));
  summary.erase(
      std::find_if(summary.rbegin(), summary.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
      summary.end());
  if (summary.size() > 72) {
    summary.resize(72);
  }
  return summary;
}

#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>
#include <strings.h>

#include <functional>
#include <vector>

#include "ClippingStore.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "util/BookmarkUtil.h"
#include "util/TaskWatchdog.h"

namespace {
constexpr size_t NAME_BUFFER_SIZE = 1024;  // FAT long names can reach ~765 UTF-8 bytes

std::string getBookCachePath(const std::string& path) {
  const char* prefix = nullptr;
  if (FsHelpers::hasReflowableBookExtension(path)) {
    prefix = "epub_";
  } else if (FsHelpers::hasXtcExtension(path)) {
    prefix = "xtc_";
  } else {
    return "";
  }
  return std::string("/.crosspoint/") + prefix + std::to_string(std::hash<std::string>{}(path));
}

// The caller made sure no book lives at the new path, so state keyed by the
// full path (cache hash, sidecars) found there is left over from a deleted
// book and is replaced.
bool moveStatePath(const std::string& oldPath, const std::string& newPath, bool& moved) {
  moved = false;
  // FAT names are case-insensitive: a path differing only in case is the same file.
  if (oldPath.empty() || strcasecmp(oldPath.c_str(), newPath.c_str()) == 0 || !Storage.exists(oldPath.c_str())) {
    return true;
  }
  if (Storage.exists(newPath.c_str())) {
    bool isDir = false;
    {
      auto stale = Storage.open(newPath.c_str());
      isDir = stale && stale.isDirectory();
    }
    LOG_DBG("BookCache", "Replacing leftover book state: %s", newPath.c_str());
    if (!(isDir ? Storage.removeDir(newPath.c_str()) : Storage.remove(newPath.c_str()))) {
      LOG_ERR("BookCache", "Failed to remove leftover book state: %s", newPath.c_str());
      return false;
    }
  }
  moved = Storage.rename(oldPath.c_str(), newPath.c_str());
  if (!moved) LOG_ERR("BookCache", "Failed to move book state: %s -> %s", oldPath.c_str(), newPath.c_str());
  return moved;
}

void rollBackStatePath(const std::string& oldPath, const std::string& newPath, const bool moved) {
  if (moved && !Storage.rename(newPath.c_str(), oldPath.c_str())) {
    LOG_ERR("BookCache", "Failed to roll back book state: %s -> %s", newPath.c_str(), oldPath.c_str());
  }
}

// Bookmark file names drop the extension and flatten folders, so another live
// book (a.txt next to a.epub, or /x_y.epub for /x/y.epub) can own the target
// name. Never replace it: the moved book just loses its bookmarks instead.
bool moveBookmarks(const std::string& oldBook, const std::string& newBook, bool& moved) {
  moved = false;
  if (!FsHelpers::hasReflowableBookExtension(oldBook)) return true;
  const std::string from = BookmarkUtil::getBookmarkPath(oldBook);
  const std::string to = BookmarkUtil::getBookmarkPath(newBook);
  if (strcasecmp(from.c_str(), to.c_str()) == 0 || !Storage.exists(from.c_str())) return true;
  if (Storage.exists(to.c_str())) {
    LOG_ERR("BookCache", "Bookmark name taken, bookmarks not moved: %s", to.c_str());
    return true;
  }
  moved = Storage.rename(from.c_str(), to.c_str());
  if (!moved) LOG_ERR("BookCache", "Failed to move bookmarks: %s -> %s", from.c_str(), to.c_str());
  return moved;
}

bool moveBookFile(const std::string& oldPath, const std::string& newPath) {
  const std::string oldCachePath = getBookCachePath(oldPath);
  const std::string newCachePath = getBookCachePath(newPath);
  const bool hasBookmarks = FsHelpers::hasReflowableBookExtension(oldPath);
  const std::string oldBookmarkPath = hasBookmarks ? BookmarkUtil::getBookmarkPath(oldPath) : "";
  const std::string newBookmarkPath = hasBookmarks ? BookmarkUtil::getBookmarkPath(newPath) : "";
  bool cacheMoved = false;
  bool bookmarksMoved = false;
  if (!moveStatePath(oldCachePath, newCachePath, cacheMoved)) return false;
  if (!moveBookmarks(oldPath, newPath, bookmarksMoved)) {
    rollBackStatePath(oldCachePath, newCachePath, cacheMoved);
    return false;
  }
  // Protected-book sidecars travel with the book; a book separated from its
  // key no longer opens.
  static constexpr const char* SIDECARS[] = {".key", ".rights"};
  bool sidecarMoved[std::size(SIDECARS)] = {};
  const auto rollBack = [&] {
    for (size_t i = 0; i < std::size(SIDECARS); i++) {
      rollBackStatePath(oldPath + SIDECARS[i], newPath + SIDECARS[i], sidecarMoved[i]);
    }
    rollBackStatePath(oldBookmarkPath, newBookmarkPath, bookmarksMoved);
    rollBackStatePath(oldCachePath, newCachePath, cacheMoved);
  };
  for (size_t i = 0; i < std::size(SIDECARS); i++) {
    if (!moveStatePath(oldPath + SIDECARS[i], newPath + SIDECARS[i], sidecarMoved[i])) {
      rollBack();
      return false;
    }
  }
  if (!ClippingStore::moveBook(oldPath, newPath)) {
    LOG_ERR("BookCache", "Failed to move file: %s -> %s", oldPath.c_str(), newPath.c_str());
    rollBack();
    return false;
  }

  RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    if (!APP_STATE.saveToFile()) LOG_ERR("BookCache", "Failed to save moved open-book path");
  }
  return true;
}

// Runs after the folder itself moved; a book whose state can't move just
// starts fresh, as it did before state was carried over.
void moveFolderBookState(const std::string& oldDir, const std::string& newDir, char* name) {
  // Folders still to visit, relative to newDir. Only pending folder names
  // are kept, so the stack stays small for book libraries.
  std::vector<std::string> pending;
  pending.emplace_back();
  while (!pending.empty()) {
    const std::string relDir = std::move(pending.back());
    pending.pop_back();
    auto dir = Storage.open((newDir + relDir).c_str());
    if (!dir || !dir.isDirectory()) continue;
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      // Large libraries take a while on SD; keep the task watchdog fed.
      resetTaskWatchdogIfSubscribed();
      const size_t length = entry.getName(name, NAME_BUFFER_SIZE);
      if (length == 0 || length >= NAME_BUFFER_SIZE) continue;  // unreadable name: that book starts fresh
      if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
      const std::string rel = relDir + "/" + name;
      if (entry.isDirectory()) {
        pending.push_back(rel);
        continue;
      }
      const std::string oldBook = oldDir + rel;
      const std::string newBook = newDir + rel;
      const std::string oldCachePath = getBookCachePath(oldBook);
      if (oldCachePath.empty()) continue;  // not a book
      const std::string newCachePath = getBookCachePath(newBook);
      bool moved = false;
      moveStatePath(oldCachePath, newCachePath, moved);
      moveBookmarks(oldBook, newBook, moved);
      RECENT_BOOKS.updatePath(oldBook, newBook, oldCachePath, newCachePath);
    }
  }
}

bool moveBookFolder(const std::string& oldPath, const std::string& newPath) {
  // ClippingStore moves the folder and every clipping store under it.
  if (!ClippingStore::moveBook(oldPath, newPath)) {
    LOG_ERR("BookCache", "Failed to move folder: %s -> %s", oldPath.c_str(), newPath.c_str());
    return false;
  }
  // Transient name buffer for the state walk, freed on return.
  auto name = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (name) {
    moveFolderBookState(oldPath, newPath, name.get());
  } else {
    LOG_ERR("BookCache", "OOM: folder state walk skipped, books in %s start fresh", newPath.c_str());
  }
  const std::string oldPrefix = oldPath + "/";
  if (APP_STATE.openEpubPath.rfind(oldPrefix, 0) == 0) {
    APP_STATE.openEpubPath = newPath + "/" + APP_STATE.openEpubPath.substr(oldPrefix.size());
    if (!APP_STATE.saveToFile()) LOG_ERR("BookCache", "Failed to save moved open-book path");
  }
  return true;
}
}  // namespace

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasReflowableBookExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

bool removeBookFile(const std::string& path) {
  if (!Storage.remove(path.c_str())) return false;
  clearBookCache(path);
  return ClippingStore::deleteForFilePath(path, "epub");
}

bool moveBookEntry(const std::string& oldPath, const std::string& newPath) {
  if (oldPath == newPath) return true;
  // FAT names are case-insensitive, so compare that way when refusing to
  // move a folder into its own subtree.
  if (newPath.size() > oldPath.size() && newPath[oldPath.size()] == '/' &&
      strncasecmp(newPath.c_str(), oldPath.c_str(), oldPath.size()) == 0) {
    LOG_ERR("BookCache", "Refused to move a folder into itself: %s -> %s", oldPath.c_str(), newPath.c_str());
    return false;
  }
  bool directory = false;
  {
    auto file = Storage.open(oldPath.c_str());
    if (!file) return false;
    directory = file.isDirectory();
  }
  return directory ? moveBookFolder(oldPath, newPath) : moveBookFile(oldPath, newPath);
}

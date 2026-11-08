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
#include "util/MirroredPath.h"
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
// full path (cache hash, bookmarks, sidecars) found there is left over from a deleted
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
  MirroredPath::ensureParent(newPath);
  moved = Storage.rename(oldPath.c_str(), newPath.c_str());
  if (!moved) LOG_ERR("BookCache", "Failed to move book state: %s -> %s", oldPath.c_str(), newPath.c_str());
  return moved;
}

void rollBackStatePath(const std::string& oldPath, const std::string& newPath, const bool moved) {
  if (!moved) return;
  MirroredPath::ensureParent(oldPath);
  if (!Storage.rename(newPath.c_str(), oldPath.c_str())) {
    LOG_ERR("BookCache", "Failed to roll back book state: %s -> %s", newPath.c_str(), oldPath.c_str());
  }
}

// Bookmark and progress paths for a book, in their mirrored folders.
struct MirroredState {
  std::string bookmarks;
  std::string progress;
};

MirroredState mirroredStateFor(const std::string& bookPath) {
  return {FsHelpers::hasReflowableBookExtension(bookPath) ? BookmarkUtil::getBookmarkPath(bookPath) : "",
          MirroredPath::progressFor(bookPath)};
}

// Moves bookmarks and progress; on failure everything moved here is put back.
bool moveMirroredState(const MirroredState& from, const MirroredState& to, bool moved[2]) {
  moved[0] = moved[1] = false;
  if (!moveStatePath(from.bookmarks, to.bookmarks, moved[0])) return false;
  if (!moveStatePath(from.progress, to.progress, moved[1])) {
    rollBackStatePath(from.bookmarks, to.bookmarks, moved[0]);
    return false;
  }
  return true;
}

void rollBackMirroredState(const MirroredState& from, const MirroredState& to, const bool moved[2]) {
  rollBackStatePath(from.progress, to.progress, moved[1]);
  rollBackStatePath(from.bookmarks, to.bookmarks, moved[0]);
}

void pruneMirroredState(const MirroredState& from, const bool moved[2]) {
  if (moved[0]) MirroredPath::removeEmptyParents(MirroredPath::BOOKMARKS_ROOT, from.bookmarks);
  if (moved[1]) MirroredPath::removeEmptyParents(MirroredPath::PROGRESS_ROOT, from.progress);
}

void adoptLegacyState(const std::string& bookPath, const std::string& cachePath) {
  if (FsHelpers::hasReflowableBookExtension(bookPath)) BookmarkUtil::adoptLegacyBookmarks(bookPath);
  MirroredPath::adoptLegacyProgress(bookPath, cachePath);
}

bool moveBookFile(const std::string& oldPath, const std::string& newPath) {
  const std::string oldCachePath = getBookCachePath(oldPath);
  const std::string newCachePath = getBookCachePath(newPath);
  adoptLegacyState(oldPath, oldCachePath);
  const MirroredState oldState = mirroredStateFor(oldPath);
  const MirroredState newState = mirroredStateFor(newPath);
  bool cacheMoved = false;
  bool stateMoved[2] = {};
  if (!moveStatePath(oldCachePath, newCachePath, cacheMoved)) return false;
  if (!moveMirroredState(oldState, newState, stateMoved)) {
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
    rollBackMirroredState(oldState, newState, stateMoved);
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

  pruneMirroredState(oldState, stateMoved);
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
      const MirroredState oldState = mirroredStateFor(oldBook);
      const MirroredState newState = mirroredStateFor(newBook);
      // Mirror folders were moved as a whole. Old-layout state is carried over
      // only when the book has no state of that kind at the new path, which is newer.
      if (!newState.bookmarks.empty() && !Storage.exists(newState.bookmarks.c_str())) {
        BookmarkUtil::adoptLegacyBookmarks(oldBook);
      }
      if (!Storage.exists(newState.progress.c_str())) MirroredPath::adoptLegacyProgress(oldBook, oldCachePath);
      bool moved = false;
      bool stateMoved[2] = {};
      moveStatePath(oldCachePath, newCachePath, moved);
      moveMirroredState(oldState, newState, stateMoved);
      pruneMirroredState(oldState, stateMoved);
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
  // Bookmarks and progress mirror the book folders, so each mirror folder
  // moves in one step; the walk below handles leftovers and old layouts.
  MirroredPath::moveFolder(MirroredPath::BOOKMARKS_ROOT, oldPath, newPath);
  MirroredPath::moveFolder(MirroredPath::PROGRESS_ROOT, oldPath, newPath);
  // Transient name buffer for the state walk, freed on return.
  auto name = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (name) {
    moveFolderBookState(oldPath, newPath, name.get());
    // Drop old mirror folders the walk emptied.
    MirroredPath::removeEmptyParents(MirroredPath::BOOKMARKS_ROOT,
                                     MirroredPath::forFolder(MirroredPath::BOOKMARKS_ROOT, oldPath) + "/");
    MirroredPath::removeEmptyParents(MirroredPath::PROGRESS_ROOT,
                                     MirroredPath::forFolder(MirroredPath::PROGRESS_ROOT, oldPath) + "/");
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
  // A replaced book starts over, as it did when progress lived in the cache.
  const std::string progress = MirroredPath::progressFor(path);
  if (Storage.exists(progress.c_str()) && Storage.remove(progress.c_str())) {
    MirroredPath::removeEmptyParents(MirroredPath::PROGRESS_ROOT, progress);
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

bool removeBookFile(const std::string& path) {
  if (!Storage.remove(path.c_str())) return false;
  clearBookCache(path);
  if (FsHelpers::hasReflowableBookExtension(path)) {
    const std::string bookmarks = BookmarkUtil::getBookmarkPath(path);
    if (Storage.exists(bookmarks.c_str()) && Storage.remove(bookmarks.c_str())) {
      MirroredPath::removeEmptyParents(MirroredPath::BOOKMARKS_ROOT, bookmarks);
    }
  }
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

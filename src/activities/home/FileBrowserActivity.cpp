#include "FileBrowserActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <functional>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "apps/Wallpaper.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "network/ProtectedPaths.h"
#include "util/BookCacheUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t NAME_BUFFER_SIZE = 500;
// First row while picking a move destination. Not a valid file name, so it
// never collides with a real entry.
const std::string MOVE_HERE_ENTRY = "\x01";

std::string joinPath(const std::string& dir, const std::string& name) {
  return dir.back() == '/' ? dir + name : dir + "/" + name;
}

}  // namespace

std::string getFileExtension(const std::string& filename);

void formatFileName(const std::string& filename, char* buffer, const size_t bufferSize) {
  if (filename.empty()) {
    buffer[0] = '\0';
    return;
  }
  const bool isDirectory = filename.back() == '/';
  const size_t dot = isDirectory ? filename.size() - 1 : filename.rfind('.');
  const int length = static_cast<int>(dot == std::string::npos ? filename.size() : dot);
  const char* format = isDirectory && !UITheme::getInstance().getTheme().showsFileIcons() ? "[%.*s]" : "%.*s";
  snprintf(buffer, bufferSize, format, length, filename.c_str());
  // Compose only the display copy; filesystem lookup needs the raw entry bytes.
  utf8ComposeNfcInPlace(buffer);
}

void formatFileExtension(const std::string& filename, char* buffer, const size_t bufferSize) {
  buffer[0] = '\0';
  if (filename.empty() || filename.back() == '/') return;
  if (const char* extension = strrchr(filename.c_str(), '.')) snprintf(buffer, bufferSize, "%s", extension);
}

FileBrowserActivity::FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::string initialPath, const Mode mode)
    : UiListActivity("FileBrowser", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      mode(mode),
      basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}

void FileBrowserActivity::loadFiles() {
  files.clear();
  prewarmedStart = -1;  // new folder contents: re-prewarm the visible window

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    return;
  }

  root.rewindDirectory();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    root.close();
    return;
  }

  // Size `files` once before the fill loop: doubling growth on a large folder
  // holds the old and new string blocks live across the final realloc. The
  // count-only pass reads no names, so filtered-out entries just over-reserve
  // by a few slots.
  size_t entryCount = 0;
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) ++entryCount;
  root.rewindDirectory();
  files.reserve(entryCount);

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    const bool isDirectory = file.isDirectory();
    if ((!SETTINGS.showHiddenFiles && fileNameBuffer[0] == '.') ||
        strcmp(fileNameBuffer.get(), "System Volume Information") == 0) {
      continue;
    }

    if (isDirectory) {
      // The folder being moved can't be its own destination.
      if (isMoving() && joinPath(basepath, fileNameBuffer.get()) == movingPath) continue;
      files.emplace_back(std::string(fileNameBuffer.get()) + "/");
    } else if (isMoving()) {
      continue;
    } else {
      std::string_view filename{fileNameBuffer.get()};
      if (mode == Mode::PickFirmware) {
        // Firmware picker: only show .bin files.
        if (FsHelpers::checkFileExtension(filename, ".bin")) {
          files.emplace_back(filename);
        }
      } else if (FsHelpers::hasReflowableBookExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                 FsHelpers::hasImageExtension(filename) || wallpaper::isOpg(filename)) {
        files.emplace_back(filename);
      }
    }
  }
  root.close();
  FsHelpers::sortFileList(files);
  if (isMoving()) files.insert(files.begin(), MOVE_HERE_ENTRY);
}

// fui::ListProps::rowProvider — formats row `index` from files[index] into the
// activity's scratch buffers on demand. Runs on the render task only for the
// rows the list actually lays out, so nothing per-file is materialized beyond
// `files` itself. The label/value pointers stay valid until the next call,
// which is all the provider contract requires. The "[folder]" format is
// theme-dependent, but it's re-derived here on every
// repaint, so a theme change picked up while this activity was paused
// underneath another screen needs no cache invalidation.
void FileBrowserActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<FileBrowserActivity*>(ctx);
  if (index >= self->files.size()) return;
  const std::string& entry = self->files[index];
  if (entry == MOVE_HERE_ENTRY) {
    item.label = tr(STR_MOVE_HERE);
    item.icon = listIconFor(UIIcon::Folder);
    item.actionValue = static_cast<int16_t>(index);
    return;
  }
  formatFileName(entry, self->rowNameBuf, sizeof(self->rowNameBuf));
  item.label = self->rowNameBuf;
  formatFileExtension(entry, self->rowExtBuf, sizeof(self->rowExtBuf));
  if (self->rowExtBuf[0] != '\0') {
    item.value = self->rowExtBuf;
  }
  item.icon = listIconFor(UITheme::getFileIcon(entry));
  item.actionValue = static_cast<int16_t>(index);
}

// Batch-prewarm the CJK fallback glyphs for a bounded window of display names
// starting at the viewport top — one SD pass per list page (the reader TOC's
// refreshTocWindow pattern) instead of one unbounded pass over the whole
// folder. Getter form: no concatenated copy (a bare-new string append aborts
// under heap pressure). The last index covers the bottom path band: basepath
// (possibly a CJK folder name) draws in the same small font, so it must live
// in the same batch or it would evict the rows' glyphs when the heap gate
// disables union merging. (prewarmFallbackText appends the truncation
// ellipsis.)
void FileBrowserActivity::prewarmRowGlyphs(const int start) {
  const int total = static_cast<int>(files.size());
  int clamped = start;
  if (clamped > total - PREWARM_WINDOW) clamped = total - PREWARM_WINDOW;
  if (clamped < 0) clamped = 0;
  if (clamped == prewarmedStart) return;
  prewarmedStart = clamped;
  const int count = total - clamped < PREWARM_WINDOW ? total - clamped : PREWARM_WINDOW;

  struct PrewarmCtx {
    FileBrowserActivity* self;
    int first;
    int count;
  } prewarmCtx{this, clamped, count};
  renderer.prewarmFallbackText(
      uiScaleSpec().smallFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        auto* c = const_cast<PrewarmCtx*>(static_cast<const PrewarmCtx*>(ctx));
        if (i < static_cast<uint32_t>(c->count)) {
          if (c->self->files[c->first + i] == MOVE_HERE_ENTRY) return tr(STR_MOVE_HERE);
          formatFileName(c->self->files[c->first + i], c->self->rowNameBuf, sizeof(c->self->rowNameBuf));
          return c->self->rowNameBuf;
        }
        return c->self->basepath.c_str();
      },
      &prewarmCtx, static_cast<uint32_t>(count) + 1);
}

void FileBrowserActivity::onEnter() {
  UiListActivity::onEnter();

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "malloc failed for name buffer");
    return;
  }

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    // The first screen build pulls the viewport to it (ListNav follow-on-build).
    nav.selected = static_cast<int>(findEntry(fileName));
  } else {
    loadFiles();
  }
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
  fileNameBuffer.reset();
}

// To avoid traversing directories twice (once for cache clearing, once for deletion),
// we do both in one pass here, instead of using Storage.removeDir
bool FileBrowserActivity::removeDirFile(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FileBrowser", "Failed to open for metadata clearing: %s", fullPath.c_str());
    return false;
  }

  if (!file.isDirectory()) {
    file.close();
    return removeBookFile(fullPath);
  }
  file.close();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    return false;
  }

  // Stack of (dirPath, postOrder): postOrder=true means rmdir this path after children are processed.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FileBrowser", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir) {
      LOG_ERR("FileBrowser", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }
    if (!dir.isDirectory()) {
      LOG_ERR("FileBrowser", "Not a directory: %s", currentPath.c_str());
      return false;
    }

    // Push this dir for post-order rmdir (after all children are processed).
    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      if (strcmp(fileNameBuffer.get(), ".") == 0 || strcmp(fileNameBuffer.get(), "..") == 0) {
        continue;
      }
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') {
        entryPath += "/";
      }
      entryPath += fileNameBuffer.get();

      const bool isDir = entry.isDirectory();
      entry.close();

      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        if (!removeBookFile(entryPath)) {
          LOG_ERR("FileBrowser", "Failed to remove file: %s", entryPath.c_str());
          return false;
        }
      }
    }
  }

  return true;
}

void FileBrowserActivity::activateIndex(const int index) {
  (void)index;  // base already synced nav.selected to the tapped row
  // Activation navigates or opens; a lingering flash would gray an unrelated
  // row on the next list.
  app.clearTapFlash();
  activateSelected();
}

void FileBrowserActivity::onRowLongPress(const int index) {
  (void)index;  // base already synced nav.selected to the pressed row
  app.clearTapFlash();
  if (mode == Mode::Books && !isMoving()) {
    showEntryActions();
  } else {
    activateSelected();
  }
}

#if FREEINK_CAP_TOUCH
void FileBrowserActivity::swipeDelete(const int index) {
  nav.selected = index;
  deleteSelected();
}
#endif

void FileBrowserActivity::activateSelected() {
  if (files.empty()) return;
  // A touch activation can carry a row index captured before a delete/reload
  // shrank the list; the next render re-registers the rows.
  if (nav.selected < 0 || nav.selected >= listCount()) return;

  const std::string& entry = files[nav.selected];
  if (entry == MOVE_HERE_ENTRY) {
    finishMove();
    return;
  }
  bool isDirectory = (entry.back() == '/');

  // Firmware picker: select file -> return path; navigate into directories normally.
  if (mode == Mode::PickFirmware && !isDirectory) {
    std::string cleanBasePath = basepath;
    if (cleanBasePath.back() != '/') cleanBasePath += "/";
    ActivityResult res{FilePathResult{cleanBasePath + entry}};
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  // buildScreen() runs on the render task and reads basepath plus `files`
  // through the row provider; mutate only under the render lock.
  RenderLock lock(*this);
  if (basepath.back() != '/') basepath += "/";

  if (isDirectory) {
    basepath += entry.substr(0, entry.length() - 1);
    loadFiles();
    nav.selected = 0;
    nav.top = 0;
    lock.unlock();
    requestUpdate();
  } else {
    const std::string fullPath = basepath + entry;
    lock.unlock();  // onSelectBook launches an activity; don't hold the lock across it
    onSelectBook(fullPath);
  }
}

void FileBrowserActivity::showEntryActions() {
  if (mode != Mode::Books || isMoving() || optionPopup.isActive()) return;

  enum Action { Open, Rename, Move, NewFolder, Delete };
  static constexpr StrId OPTIONS[] = {StrId::STR_OPEN, StrId::STR_RENAME, StrId::STR_MOVE, StrId::STR_NEW_FOLDER,
                                      StrId::STR_DELETE};
  // An empty folder has no entry to act on, but can still get a new folder.
  const bool hasEntry = !files.empty() && nav.selected >= 0 && nav.selected < listCount();
  const int first = hasEntry ? Open : NewFolder;
  const int count = hasEntry ? static_cast<int>(std::size(OPTIONS)) : 1;
  optionPopup.show(StrId::STR_FILENAME, OPTIONS + first, count, 0, [this, first](const int index) {
    switch (first + index) {
      case Open:
        activateSelected();
        break;
      case Rename:
        startRename();
        break;
      case Move:
        startMove();
        break;
      case NewFolder:
        startNewFolder();
        break;
      case Delete:
        deleteSelected();
        break;
      default:
        break;
    }
  });
  requestUpdate();
}

std::string FileBrowserActivity::currentDir() const { return basepath.back() == '/' ? basepath : basepath + "/"; }

void FileBrowserActivity::reloadAndSelect(const std::string& entry) {
  {
    RenderLock lock(*this);
    loadFiles();
    nav.selected = static_cast<int>(findEntry(entry));
    nav.follow(listCount());
  }
  requestUpdate(true);
}

void FileBrowserActivity::showFileActionError(const StrId message) {
  const char* options[] = {tr(STR_OK_BUTTON)};
  optionPopup.show(tr(STR_FILE_ACTION_FAILED), I18N.get(message), options, 1, 0, [](int) {});
  requestUpdate(true);
}

void FileBrowserActivity::deleteSelected() {
  if (files.empty() || nav.selected < 0 || nav.selected >= listCount()) return;

  std::string cleanBasePath = basepath;
  if (cleanBasePath.back() != '/') cleanBasePath += "/";
  const std::string entry = files[nav.selected];
  const std::string fullPath = cleanBasePath + entry;

  auto handler = [this, fullPath](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("FileBrowser", "Delete cancelled by user");
      return;
    }

    LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
    if (!removeDirFile(fullPath)) {
      LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
      return;
    }

    LOG_DBG("FileBrowser", "Deleted successfully");
    {
      RenderLock lock(*this);
      loadFiles();
      if (files.empty()) {
        nav.selected = 0;
      } else if (nav.selected >= listCount()) {
        nav.selected = listCount() - 1;
      }
      nav.follow(listCount());
    }
    requestUpdate(true);
  };

  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_ITEM_CONFIRM),
                                                              utf8ComposeNfc(entry));
  if (!confirmation) {
    LOG_ERR("FileBrowser", "OOM: delete confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation), std::move(handler));
}

void FileBrowserActivity::startRename() {
  if (files.empty() || nav.selected < 0 || nav.selected >= listCount()) return;

  const std::string oldEntry = files[nav.selected];
  const bool isDirectory = oldEntry.back() == '/';
  const std::string oldPath = currentDir() + (isDirectory ? oldEntry.substr(0, oldEntry.size() - 1) : oldEntry);
  const std::string extension = getFileExtension(oldEntry);
  const size_t stemLength = oldEntry.size() - (isDirectory ? 1 : extension.size());
  const std::string initialStem = utf8ComposeNfc(oldEntry.substr(0, stemLength));
  const size_t maxStemLength = NAME_BUFFER_SIZE - extension.size() - 1;
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RENAME), initialStem,
                                                           maxStemLength, InputType::Text);
  if (!keyboard) {
    LOG_ERR("FileBrowser", "OOM: rename keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this, oldPath, oldEntry, extension](const ActivityResult& result) {
    if (result.isCancelled) return;
    renameSelected(oldPath, oldEntry, std::get<KeyboardResult>(result.data).text, extension);
  });
}

void FileBrowserActivity::renameSelected(const std::string& oldPath, const std::string& oldEntry,
                                         const std::string& newStem, const std::string& extension) {
  const bool isDirectory = oldEntry.back() == '/';
  const std::string newName = newStem + extension;
  if (newStem.empty() || newName.size() >= NAME_BUFFER_SIZE || !FsHelpers::isSafePathComponent(newName)) {
    LOG_ERR("FileBrowser", "Invalid rename target: %s", newName.c_str());
    showFileActionError(StrId::STR_INVALID_NAME);
    return;
  }
  const std::string newEntry = isDirectory ? newName + "/" : newName;
  if (newEntry == utf8ComposeNfc(oldEntry)) return;

  const std::string newPath = joinPath(FsHelpers::extractFolderPath(oldPath), newName);
  if (Storage.exists(newPath.c_str())) {
    LOG_ERR("FileBrowser", "Rename target already exists: %s", newPath.c_str());
    showFileActionError(StrId::STR_NAME_TAKEN);
    return;
  }
  if (!relocateEntry(oldPath, newPath)) {
    showFileActionError(StrId::STR_FILE_CHANGE_FAILED_HINT);
    return;
  }
  reloadAndSelect(newEntry);
}

void FileBrowserActivity::startMove() {
  if (files.empty() || nav.selected < 0 || nav.selected >= listCount()) return;
  const std::string& entry = files[nav.selected];
  const bool isDirectory = entry.back() == '/';
  {
    RenderLock lock(*this);
    movingPath = currentDir() + (isDirectory ? entry.substr(0, entry.size() - 1) : entry);
    loadFiles();
    nav.selected = 0;
    nav.top = 0;
  }
  requestUpdate(true);
}

// Leaves move mode and returns to the moved entry's folder.
void FileBrowserActivity::cancelMove() {
  const std::string name = movingPath.substr(movingPath.find_last_of('/') + 1);
  std::string selectEntry = name;
  {
    auto file = Storage.open(movingPath.c_str());
    if (file && file.isDirectory()) selectEntry += "/";
  }
  {
    RenderLock lock(*this);
    basepath = FsHelpers::extractFolderPath(movingPath);
    movingPath.clear();
  }
  reloadAndSelect(selectEntry);
}

void FileBrowserActivity::finishMove() {
  const std::string oldPath = movingPath;
  const std::string name = oldPath.substr(oldPath.find_last_of('/') + 1);
  const std::string newPath = joinPath(basepath, name);
  bool isDirectory = false;
  {
    auto file = Storage.open(oldPath.c_str());
    if (!file) {
      LOG_ERR("FileBrowser", "Move source vanished: %s", oldPath.c_str());
      cancelMove();
      return;
    }
    isDirectory = file.isDirectory();
  }

  if (newPath == oldPath) {
    cancelMove();
    return;
  }
  // The picker hides the moved folder; this guards against moving it into itself anyway.
  if (isDirectory && newPath.rfind(oldPath + "/", 0) == 0) {
    showFileActionError(StrId::STR_FILE_CHANGE_FAILED_HINT);
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    LOG_ERR("FileBrowser", "Move target already exists: %s", newPath.c_str());
    showFileActionError(StrId::STR_NAME_TAKEN);
    return;
  }
  if (!relocateEntry(oldPath, newPath)) {
    showFileActionError(StrId::STR_FILE_CHANGE_FAILED_HINT);
    return;
  }

  // Stay in the destination folder with the moved entry selected.
  {
    RenderLock lock(*this);
    movingPath.clear();
  }
  reloadAndSelect(isDirectory ? name + "/" : name);
}

void FileBrowserActivity::startNewFolder() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_NEW_FOLDER), "",
                                                           NAME_BUFFER_SIZE - 1, InputType::Text);
  if (!keyboard) {
    LOG_ERR("FileBrowser", "OOM: new folder keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    createFolder(std::get<KeyboardResult>(result.data).text);
  });
}

void FileBrowserActivity::createFolder(const std::string& name) {
  if (name.empty() || name.size() >= NAME_BUFFER_SIZE || !FsHelpers::isSafePathComponent(name)) {
    showFileActionError(StrId::STR_INVALID_NAME);
    return;
  }
  const std::string path = currentDir() + name;
  if (Storage.exists(path.c_str())) {
    showFileActionError(StrId::STR_NAME_TAKEN);
    return;
  }
  if (!Storage.mkdir(path.c_str())) {
    LOG_ERR("FileBrowser", "Failed to create folder: %s", path.c_str());
    showFileActionError(StrId::STR_FILE_CHANGE_FAILED_HINT);
    return;
  }
  reloadAndSelect(name + "/");
}

bool FileBrowserActivity::relocateEntry(const std::string& oldPath, const std::string& newPath) {
  // Hidden entries such as /.crosspoint hold device state; moving them would
  // scatter settings and progress (they only show with Show Hidden Files).
  const auto hiddenName = [](const std::string& path) { return path.find("/.") != std::string::npos; };
  if (hiddenName(oldPath) || hiddenName(newPath) || protectedpaths::isSensitivePath(oldPath.c_str()) ||
      protectedpaths::isSensitivePath(newPath.c_str())) {
    LOG_ERR("FileBrowser", "Refused to move protected path: %s", oldPath.c_str());
    return false;
  }
  return moveBookEntry(oldPath, newPath);
}

bool FileBrowserActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  // Long press BACK (1s+) goes to root folder (Books mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  if (mode == Mode::Books && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS && basepath != "/") {
    {
      // buildScreen() runs on the render task and reads basepath plus `files`
      // through the row provider; mutate only under the render lock.
      RenderLock lock(*this);
      basepath = "/";
      loadFiles();
      nav.selected = 0;
      nav.top = 0;
    }
    requestUpdate();
    return true;
  }

  return false;
}

bool FileBrowserActivity::handleButtons() {
  if (mode == Mode::Books && !isMoving() &&
      mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, GO_HOME_MS)) {
    app.clearTapFlash();
    showEntryActions();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        {
          // buildScreen() runs on the render task and reads basepath plus `files`
          // through the row provider; mutate only under the render lock.
          RenderLock lock(*this);
          basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
          if (basepath.empty()) basepath = "/";
          loadFiles();

          const auto pos = oldPath.find_last_of('/');
          const std::string dirName = oldPath.substr(pos + 1) + "/";
          nav.selected = static_cast<int>(findEntry(dirName));
          nav.top = 0;
          nav.follow(listCount());
        }

        requestUpdate();
      } else if (isMoving()) {
        cancelMove();
      } else if (mode == Mode::PickFirmware) {
        // Firmware picker at root: cancel back to caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        onGoHome();
      }
    }
    return true;
  }

  return false;
}

void FileBrowserActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

std::string getFileExtension(const std::string& filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void FileBrowserActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Full path band at the bottom: separator on top, left-truncated so the
  // deepest directory stays visible.
  {
    const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(pathLineHeight + metrics.verticalSpacing));
    screen.target().fill(fui::Rect{band.x, band.y, band.width, 3}, fui::Paint::solid(fui::Color::Black));
    const int pathY =
        band.y + metrics.verticalSpacing / 2 + (band.height - metrics.verticalSpacing / 2 - pathLineHeight) / 2;
    const int pathMaxWidth = band.width - metrics.contentSidePadding * 2;
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, band.x + metrics.contentSidePadding, pathY, pathDisplay);
  }

  if (files.empty()) {
    screen.centeredText(mode == Mode::PickFirmware ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND),
                        screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.rowProvider = &FileBrowserActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(files.size());
  props.action = ACTION_ROW;
  // Tap opens/navigates; long-press shows entry actions (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;  // air between the extension and the row edge
  // Names use up to two small-font lines; shared list layout sizes each row.
  fui::TextStyle label = screen.theme().smallText;
  label.maxLines = 2;
  props.labelText = label;

  // The trailing value here is just the short extension: skip the balanced
  // 60%-band wrap cap and let both name lines run the full width before it.
  props.balanceWrappedLabelWithValue = false;
  syncListViewport(screen, props);
  // Prewarm the window at the final viewport (syncListViewport just applied
  // follow/clamping to nav.top) before the list resolves rows through the
  // provider.
  prewarmRowGlyphs(nav.top);
#if FREEINK_CAP_TOUCH
  if (mode == Mode::Books && !isMoving()) configureSwipeDelete(screen, props);
#endif
  screen.list(props);
}

void FileBrowserActivity::drawChrome() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName =
      (mode == Mode::PickFirmware)
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));
  if (isMoving()) folderName = std::string(tr(STR_MOVE_TO)) + ": " + folderName;
  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());
}

void FileBrowserActivity::drawFooter() {
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  if (isMoving() && basepath == "/") backLabel = tr(STR_CANCEL);
  // In PickFirmware mode, Confirm on a .bin returns the path to the caller (not "open"); show
  // STR_SELECT instead. Directories in the same picker still descend, so keep STR_OPEN there.
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && nav.selected >= 0 &&
                                     nav.selected < listCount() && files[nav.selected].back() != '/';
  const bool onMoveHere = isMoving() && nav.selected == 0;
  const char* confirmLabel =
      files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : (onMoveHere ? tr(STR_MOVE) : tr(STR_OPEN)));
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, files.empty() ? "" : tr(STR_DIR_UP),
                                            files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  const auto entry = std::find(files.begin(), files.end(), name);
  return entry != files.end() ? static_cast<size_t>(entry - files.begin()) : 0;
}

#include "MirroredPath.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <functional>
#include <vector>

#include "util/TaskWatchdog.h"

namespace MirroredPath {

std::string forBook(const char* root, const std::string& bookPath, const char* suffix) {
  std::string path = forFolder(root, bookPath);
  // A FAT long name stops at 255 characters; keep a readable prefix and make a
  // too-long name unique with a hash of the file name. Only the name is hashed
  // so a folder rename keeps the mirrored file valid.
  constexpr size_t MAX_NAME = 245;
  const size_t nameStart = path.find_last_of('/') + 1;
  if (path.size() - nameStart > MAX_NAME) {
    const std::string name = path.substr(nameStart);
    path.resize(nameStart + MAX_NAME - 21);
    while ((static_cast<unsigned char>(path.back()) & 0xC0) == 0x80) path.pop_back();  // keep UTF-8 whole
    if (static_cast<unsigned char>(path.back()) >= 0xC0) path.pop_back();
    path += "~" + std::to_string(std::hash<std::string>{}(name));
  }
  return path + suffix;
}

std::string forFolder(const char* root, const std::string& bookFolder) {
  std::string path = root;
  if (!bookFolder.empty() && bookFolder[0] != '/') path += '/';
  path += bookFolder;
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  return path;
}

void adoptLegacyProgress(const std::string& bookPath, const std::string& cachePath) {
  if (cachePath.empty()) return;
  const std::string legacy = cachePath + "/progress.bin";
  if (!Storage.exists(legacy.c_str())) return;
  const std::string path = progressFor(bookPath);
  if (Storage.exists(path.c_str())) {
    Storage.remove(legacy.c_str());  // the mirrored copy is newer
    return;
  }
  ensureParent(path);
  if (!Storage.rename(legacy.c_str(), path.c_str())) {
    LOG_ERR("MIRROR", "Failed to adopt legacy progress: %s", legacy.c_str());
  }
}

bool removeTree(const char* root) {
  if (!Storage.exists(root)) return true;
  // Names can be up to 255 UTF-16 units; 1024 bytes holds any of them as UTF-8.
  constexpr size_t NAME_BUFFER_SIZE = 1024;
  auto name = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!name) {
    LOG_ERR("MIRROR", "OOM: cannot remove %s", root);
    return false;
  }
  bool ok = true;
  std::vector<std::string> pending{root};
  std::vector<std::string> emptied;  // parents before children
  while (!pending.empty()) {
    const std::string dirPath = std::move(pending.back());
    pending.pop_back();
    {
      auto dir = Storage.open(dirPath.c_str());
      if (!dir || !dir.isDirectory()) {
        ok = false;
        continue;
      }
      for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        resetTaskWatchdogIfSubscribed();
        const size_t length = entry.getName(name.get(), NAME_BUFFER_SIZE);
        if (length == 0 || length >= NAME_BUFFER_SIZE) {
          ok = false;
          continue;
        }
        if (strcmp(name.get(), ".") == 0 || strcmp(name.get(), "..") == 0) continue;
        const std::string path = dirPath + "/" + name.get();
        if (entry.isDirectory()) {
          pending.push_back(path);
        } else {
          entry.close();
          ok = Storage.remove(path.c_str()) && ok;
        }
      }
    }
    emptied.push_back(dirPath);
  }
  for (auto it = emptied.rbegin(); it != emptied.rend(); ++it) ok = Storage.rmdir(it->c_str()) && ok;
  return ok;
}

void ensureParent(const std::string& path) {
  const std::string parent = path.substr(0, path.find_last_of('/'));
  if (!parent.empty() && !Storage.exists(parent.c_str())) Storage.mkdir(parent.c_str(), true);
}

void removeEmptyParents(const char* root, const std::string& path) {
  const std::string rootPath = root;
  std::string folder = path.substr(0, path.find_last_of('/'));
  while (folder.size() > rootPath.size() && folder.compare(0, rootPath.size(), rootPath) == 0) {
    {
      // Check explicitly: rmdir is not guaranteed to refuse a folder in use.
      auto dir = Storage.open(folder.c_str());
      if (!dir || !dir.isDirectory()) return;
      char name[4];
      for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        entry.getName(name, sizeof(name));
        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) return;
      }
    }
    if (!Storage.rmdir(folder.c_str())) return;
    folder.erase(folder.find_last_of('/'));
  }
}

void moveFolder(const char* root, const std::string& oldFolder, const std::string& newFolder) {
  const std::string from = forFolder(root, oldFolder);
  const std::string to = forFolder(root, newFolder);
  if (!Storage.exists(from.c_str()) || Storage.exists(to.c_str())) return;
  ensureParent(to);
  if (Storage.rename(from.c_str(), to.c_str())) {
    removeEmptyParents(root, from);
  } else {
    LOG_ERR("MIRROR", "Failed to move state folder: %s -> %s", from.c_str(), to.c_str());
  }
}

}  // namespace MirroredPath

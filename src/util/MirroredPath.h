#pragma once

#include <string>

// Per-book state kept in folders under /.crosspoint that mirror the book
// folders: /Books/X.epub has /.crosspoint/<root>/Books/X.epub<suffix>.
namespace MirroredPath {

inline constexpr const char* BOOKMARKS_ROOT = "/.crosspoint/bookmarks";
inline constexpr const char* PROGRESS_ROOT = "/.crosspoint/progress";

std::string forBook(const char* root, const std::string& bookPath, const char* suffix);
std::string forFolder(const char* root, const std::string& bookFolder);

inline std::string progressFor(const std::string& bookPath) { return forBook(PROGRESS_ROOT, bookPath, ".bin"); }

// Older firmware kept progress in the book cache folder; move it to the
// mirrored path the first time the book is read or moved.
void adoptLegacyProgress(const std::string& bookPath, const std::string& cachePath);

// Deletes `root` and everything under it. Unlike Storage.removeDir this
// handles names up to the FAT limit.
bool removeTree(const char* root);

// Creates the folders above `path`.
void ensureParent(const std::string& path);
// Removes folders above `path` that are empty, stopping at `root`.
void removeEmptyParents(const char* root, const std::string& path);
// Moves the mirror of a book folder in one rename when the target is free.
void moveFolder(const char* root, const std::string& oldFolder, const std::string& newFolder);

}  // namespace MirroredPath

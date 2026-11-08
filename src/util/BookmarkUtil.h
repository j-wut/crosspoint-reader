#pragma once
#include <string>

class BookmarkUtil {
 public:
  static std::string getBookmarksDir();
  // One file per book; the bookmarks folder mirrors the book folders.
  static std::string getBookmarkPath(const std::string& bookPath);
  // Renames a bookmark file saved under the old shared naming (folders
  // flattened to "_", extension dropped) to this book's own name.
  static void adoptLegacyBookmarks(const std::string& bookPath);
  static std::string sanitizeBookmarkSummary(std::string summary);
};

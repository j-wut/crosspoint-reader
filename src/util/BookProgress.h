#pragma once

#include <string>

// Saved reading position of a book.
struct BookProgressInfo {
  int percent = -1;     // -1 when unavailable
  std::string chapter;  // EPUB chapter title, empty when unknown
  int page = 0;         // 1-based page in the chapter, 0 when unknown
  int chapterPages = 0;
};

// Saved reading percentage, or -1 when unavailable. Never builds reading caches.
int loadBookProgress(const std::string& path);
// Same, plus the chapter title and page for EPUBs.
BookProgressInfo loadBookProgressInfo(const std::string& path, bool withChapter = true);

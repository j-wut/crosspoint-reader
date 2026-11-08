#pragma once

#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);

// Delete a local book and its local clipping state after the file is removed.
bool removeBookFile(const std::string& path);

// Move a book file or a folder of books, carrying the reading state (cache
// and progress, bookmarks, clippings, recent list, open book) of every book
// to its new path. The caller checks that newPath is free and allowed.
bool moveBookEntry(const std::string& oldPath, const std::string& newPath);

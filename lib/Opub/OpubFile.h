#pragma once

#include <string>

namespace opub {

// Converts the .opub at `opubPath` into an EPUB at `epubPath` unless an
// up-to-date copy is already there. The EPUB is written to a temporary file
// first, so a failed run never leaves a partial book.
bool ensureEpub(const std::string& opubPath, const std::string& epubPath);

}  // namespace opub

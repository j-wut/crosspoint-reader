#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Converts a BOOX ".opub" book (the format the BOOX Tiles app sends to the
// Picco) into a plain EPUB that the normal reader opens.
//
// An .opub file is one FlatBuffer: a header table points at sections for
// metadata, table of contents, chapters, styles and images. Each chapter holds
// blocks (paragraphs) that list elements (text runs, links, line breaks,
// images). Images are stored as raw grayscale pixels.
namespace opub {

class Source {
 public:
  virtual ~Source() = default;
  virtual uint32_t size() = 0;
  virtual bool readAt(uint32_t offset, uint8_t* dst, size_t len) = 0;
};

// Output file. The zip writer seeks back to fill in each entry's size and CRC.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual bool write(const uint8_t* data, size_t len) = 0;
  virtual bool seek(uint32_t position) = 0;
  virtual uint32_t position() = 0;
};

struct Result {
  bool ok = false;
  std::string error;  // set when !ok
  std::string title;
  int chapters = 0;
  int images = 0;
};

// `fallbackTitle` is used when the book has no title.
Result convertToEpub(Source& in, Sink& out, const std::string& fallbackTitle);

bool hasOpubExtension(const char* path);

}  // namespace opub

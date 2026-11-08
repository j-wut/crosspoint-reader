#include "OpubFile.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include "OpubToEpub.h"

namespace opub {
namespace {

class FileSource : public Source {
 public:
  explicit FileSource(HalFile& f) : f_(f), size_(static_cast<uint32_t>(f.fileSize())) {}
  uint32_t size() override { return size_; }
  bool readAt(uint32_t offset, uint8_t* dst, size_t len) override {
    if (!f_.seekSet(offset)) return false;
    return f_.read(dst, len) == static_cast<int>(len);
  }

 private:
  HalFile& f_;
  uint32_t size_;
};

class FileSink : public Sink {
 public:
  explicit FileSink(HalFile& f) : f_(f) {}
  bool write(const uint8_t* data, size_t len) override { return f_.write(data, len) == len; }
  bool seek(uint32_t position) override { return f_.seekSet(position); }
  uint32_t position() override { return static_cast<uint32_t>(f_.position()); }

 private:
  HalFile& f_;
};

// The stamp records which source the EPUB came from.
std::string stampFor(HalFile& src) {
  return std::to_string(src.fileSize()) + ":" + std::to_string(src.modificationTime()) + ":v1";
}

}  // namespace

bool ensureEpub(const std::string& opubPath, const std::string& epubPath) {
  HalFile src;
  if (!Storage.openFileForRead("OPUB", opubPath, src)) return false;
  const std::string stamp = stampFor(src);
  const std::string stampPath = epubPath + ".src";

  if (Storage.exists(epubPath.c_str())) {
    HalFile s;
    if (Storage.openFileForRead("OPUB", stampPath, s)) {
      char buf[64] = {0};
      const int n = s.read(buf, sizeof(buf) - 1);
      s.close();
      if (n > 0 && stamp == std::string(buf, n)) return true;
    }
  }

  const std::string tmpPath = epubPath + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite("OPUB", tmpPath, out)) return false;
  const unsigned long started = millis();
  FileSource in(src);
  FileSink sink(out);
  std::string title = opubPath.substr(opubPath.find_last_of('/') + 1);
  if (title.size() > 5) title.resize(title.size() - 5);
  const Result r = convertToEpub(in, sink, title);
  out.close();
  src.close();
  if (!r.ok) {
    LOG_ERR("OPUB", "convert %s failed: %s", opubPath.c_str(), r.error.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.replaceFile(tmpPath.c_str(), epubPath.c_str())) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  HalFile s;
  if (Storage.openFileForWrite("OPUB", stampPath, s)) {
    s.write(stamp.data(), stamp.size());
    s.close();
  }
  LOG_INF("OPUB", "converted %s: %d chapters, %d images, %lu ms", opubPath.c_str(), r.chapters, r.images,
          millis() - started);
  return true;
}

}  // namespace opub

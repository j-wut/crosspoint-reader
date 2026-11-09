#include "OpubToEpub.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace opub {
namespace {

// Header table fields: (offset, size) structs that point at each section.
constexpr int SEC_META = 4;
constexpr int SEC_TOC = 5;
constexpr int SEC_CHAPTERS = 6;
constexpr int SEC_STYLES = 7;
constexpr int SEC_IMAGES = 8;
constexpr uint32_t MAGIC = 0x45504642;  // "BFPE"

enum ElementType : uint8_t { EL_TEXT = 1, EL_IMAGE = 2, EL_BREAK = 3, EL_LINK = 4 };

// Random access FlatBuffer reader with a small block cache: the converter
// reads many 2-4 byte fields close to each other.
class Fb {
 public:
  explicit Fb(Source& src) : src_(src), size_(src.size()) {}

  bool ok() const { return ok_; }
  uint32_t size() const { return size_; }

  uint8_t u8(uint32_t o) {
    uint8_t v = 0;
    get(o, &v, 1);
    return v;
  }
  uint16_t u16(uint32_t o) {
    uint8_t b[2] = {0, 0};
    get(o, b, 2);
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
  }
  uint32_t u32(uint32_t o) {
    uint8_t b[4] = {0, 0, 0, 0};
    get(o, b, 4);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
           (static_cast<uint32_t>(b[3]) << 24);
  }
  float f32(uint32_t o) {
    const uint32_t v = u32(o);
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
  }

  // Position of field k of the table at t, or 0 when the field is absent.
  uint32_t field(uint32_t t, int k) {
    if (!t) return 0;
    const uint32_t vt = t - static_cast<uint32_t>(static_cast<int32_t>(u32(t)));
    const uint16_t vsize = u16(vt);
    if (vsize < 4 || vsize > 1024) {
      fail();
      return 0;
    }
    const uint32_t slot = 4 + 2 * static_cast<uint32_t>(k);
    if (slot + 2 > vsize) return 0;
    const uint16_t fo = u16(vt + slot);
    return fo ? t + fo : 0;
  }
  // Follows the unsigned offset stored at pos.
  uint32_t deref(uint32_t pos) { return pos ? pos + u32(pos) : 0; }
  uint32_t root(uint32_t bufferStart) { return bufferStart + u32(bufferStart); }

  std::string str(uint32_t pos) {
    std::string s;
    const uint32_t at = deref(pos);
    if (!at) return s;
    const uint32_t len = u32(at);
    if (len > 1u << 20 || at + 4 + len > size_) {
      fail();
      return s;
    }
    s.resize(len);
    if (len && !src_.readAt(at + 4, reinterpret_cast<uint8_t*>(&s[0]), len)) fail();
    return s;
  }

  uint32_t fieldU32(uint32_t t, int k, uint32_t def = 0) {
    const uint32_t p = field(t, k);
    return p ? u32(p) : def;
  }
  uint16_t fieldU16(uint32_t t, int k, uint16_t def = 0) {
    const uint32_t p = field(t, k);
    return p ? u16(p) : def;
  }
  uint8_t fieldU8(uint32_t t, int k, uint8_t def = 0) {
    const uint32_t p = field(t, k);
    return p ? u8(p) : def;
  }
  std::string fieldStr(uint32_t t, int k) {
    const uint32_t p = field(t, k);
    return p ? str(p) : std::string();
  }
  // A struct field holding (offset, size).
  bool fieldRange(uint32_t t, int k, uint32_t& off, uint32_t& len) {
    const uint32_t p = field(t, k);
    if (!p) return false;
    off = u32(p);
    len = u32(p + 4);
    return true;
  }

  bool readRaw(uint32_t o, uint8_t* dst, size_t len) {
    if (o > size_ || len > size_ - o) {
      fail();
      return false;
    }
    if (!src_.readAt(o, dst, len)) {
      fail();
      return false;
    }
    return true;
  }

 private:
  static constexpr uint32_t BLOCK = 1024;
  static constexpr int BLOCKS = 4;

  void fail() { ok_ = false; }

  void get(uint32_t o, uint8_t* dst, size_t len) {
    if (o >= size_ || len > size_ - o) {
      fail();
      return;
    }
    while (len) {
      const uint32_t base = o - (o % BLOCK);
      const uint8_t* data = block(base);
      if (!data) {
        fail();
        return;
      }
      const size_t n = std::min<size_t>(len, base + BLOCK - o);
      memcpy(dst, data + (o - base), n);
      dst += n;
      o += static_cast<uint32_t>(n);
      len -= n;
    }
  }

  const uint8_t* block(uint32_t base) {
    for (int i = 0; i < BLOCKS; i++) {
      if (cache_[i].valid && cache_[i].base == base) return cache_[i].data;
    }
    Slot& s = cache_[next_];
    next_ = (next_ + 1) % BLOCKS;
    const uint32_t n = std::min(BLOCK, size_ - base);
    memset(s.data, 0, BLOCK);
    s.valid = src_.readAt(base, s.data, n);
    s.base = base;
    return s.valid ? s.data : nullptr;
  }

  struct Slot {
    uint32_t base = 0;
    bool valid = false;
    uint8_t data[BLOCK];
  };

  Source& src_;
  uint32_t size_;
  bool ok_ = true;
  Slot cache_[BLOCKS];
  int next_ = 0;
};

// ---- stored (uncompressed) zip writer ----

uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
  static const uint32_t table[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                     0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                     0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    crc = (crc >> 4) ^ table[crc & 15];
    crc = (crc >> 4) ^ table[crc & 15];
  }
  return ~crc;
}

class ZipWriter {
 public:
  explicit ZipWriter(Sink& out) : out_(out) {}

  bool ok() const { return ok_; }

  void begin(const std::string& name) {
    Entry e;
    e.name = name;
    e.offset = out_.position();
    entries_.push_back(e);
    u32(0x04034b50);
    u16(20);
    u16(0x0800);  // UTF-8 names
    u16(0);       // stored
    u16(0);       // time
    u16(0x5821);  // date 2024-01-01
    u32(0);       // crc, size: filled in by end()
    u32(0);
    u32(0);
    u16(static_cast<uint16_t>(name.size()));
    u16(0);
    raw(reinterpret_cast<const uint8_t*>(name.data()), name.size());
    crc_ = 0;
    size_ = 0;
  }

  void write(const uint8_t* p, size_t n) {
    crc_ = crc32Update(crc_, p, n);
    size_ += static_cast<uint32_t>(n);
    raw(p, n);
  }
  void write(const std::string& s) { write(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

  void end() {
    Entry& e = entries_.back();
    e.crc = crc_;
    e.size = size_;
    const uint32_t here = out_.position();
    if (!out_.seek(e.offset + 14)) ok_ = false;
    u32(e.crc);
    u32(e.size);
    u32(e.size);
    if (!out_.seek(here)) ok_ = false;
  }

  void finish() {
    const uint32_t cdStart = out_.position();
    for (const Entry& e : entries_) {
      u32(0x02014b50);
      u16(20);
      u16(20);
      u16(0x0800);
      u16(0);
      u16(0);
      u16(0x5821);
      u32(e.crc);
      u32(e.size);
      u32(e.size);
      u16(static_cast<uint16_t>(e.name.size()));
      u16(0);
      u16(0);
      u16(0);
      u16(0);
      u32(0);
      u32(e.offset);
      raw(reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size());
    }
    const uint32_t cdSize = out_.position() - cdStart;
    u32(0x06054b50);
    u16(0);
    u16(0);
    u16(static_cast<uint16_t>(entries_.size()));
    u16(static_cast<uint16_t>(entries_.size()));
    u32(cdSize);
    u32(cdStart);
    u16(0);
  }

 private:
  struct Entry {
    std::string name;
    uint32_t offset = 0;
    uint32_t crc = 0;
    uint32_t size = 0;
  };

  void raw(const uint8_t* p, size_t n) {
    if (n && !out_.write(p, n)) ok_ = false;
  }
  void u16(uint16_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    raw(b, 2);
  }
  void u32(uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                          static_cast<uint8_t>(v >> 24)};
    raw(b, 4);
  }

  Sink& out_;
  std::vector<Entry> entries_;
  uint32_t crc_ = 0;
  uint32_t size_ = 0;
  bool ok_ = true;
};

// Buffers small writes into one zip entry.
class EntryText {
 public:
  explicit EntryText(ZipWriter& zip) : zip_(zip) {}
  ~EntryText() { flush(); }
  EntryText& operator<<(const std::string& s) {
    buf_ += s;
    if (buf_.size() >= 2048) flush();
    return *this;
  }
  EntryText& operator<<(const char* s) { return *this << std::string(s); }
  void flush() {
    if (!buf_.empty()) zip_.write(buf_);
    buf_.clear();
  }

 private:
  ZipWriter& zip_;
  std::string buf_;
};

std::string escapeXml(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        // Drop control characters that XML 1.0 does not allow.
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t') break;
        out += c;
    }
  }
  return out;
}

std::string num(float v) {
  char b[24];
  snprintf(b, sizeof b, "%.3g", static_cast<double>(v));
  return b;
}

// ---- styles ----
// Style tables store each CSS property as a (flag byte, value) field pair.
// A length value is a struct of (float, unit).
constexpr int ST_FONT_SIZE = 5;
constexpr int ST_FONT_WEIGHT = 7;  // 1 normal, 2+ bold
constexpr int ST_FONT_STYLE = 9;   // 1 normal, 2 italic
constexpr int ST_TEXT_ALIGN = 19;  // 3 center, 4 justify
constexpr int ST_TEXT_INDENT = 21;
constexpr int ST_MARGIN_TOP = 23;
constexpr int ST_MARGIN_LEFT = 29;
constexpr int ST_MARGIN_BOTTOM = 27;

enum Unit : uint32_t { U_NUMBER = 1, U_PX = 2, U_EM = 3, U_PERCENT = 5 };

struct Length {
  bool set = false;
  float value = 0;
  uint32_t unit = 0;
};

struct Style {
  Length fontSize;
  bool bold = false;
  bool italic = false;
  uint8_t align = 0;
  Length indent, marginTop, marginBottom, marginLeft;

  // Font size in em, 1 when unknown.
  float sizeEm() const {
    if (!fontSize.set) return 1;
    switch (fontSize.unit) {
      case U_EM:
      case U_NUMBER:
        return fontSize.value;
      case U_PERCENT:
        return fontSize.value / 100;
      case U_PX:
        return fontSize.value / 16;
      default:
        return 1;
    }
  }
};

Length readLength(Fb& fb, uint32_t t, int k) {
  Length l;
  const uint32_t p = fb.field(t, k);
  if (!p) return l;
  l.value = fb.f32(p);
  l.unit = fb.u32(p + 4);
  l.set = l.unit == U_NUMBER || l.unit == U_PX || l.unit == U_EM || l.unit == U_PERCENT;
  return l;
}

std::string cssLength(const Length& l) {
  switch (l.unit) {
    case U_PX:
      return num(l.value) + "px";
    case U_EM:
      return num(l.value) + "em";
    case U_PERCENT:
      return num(l.value) + "%";
    default:
      return num(l.value);
  }
}

Style readStyle(Fb& fb, uint32_t t) {
  Style s;
  s.fontSize = readLength(fb, t, ST_FONT_SIZE);
  s.bold = fb.fieldU8(t, ST_FONT_WEIGHT, 1) >= 2;
  s.italic = fb.fieldU8(t, ST_FONT_STYLE, 1) == 2;
  s.align = fb.fieldU8(t, ST_TEXT_ALIGN, 0);
  s.indent = readLength(fb, t, ST_TEXT_INDENT);
  s.marginTop = readLength(fb, t, ST_MARGIN_TOP);
  s.marginBottom = readLength(fb, t, ST_MARGIN_BOTTOM);
  s.marginLeft = readLength(fb, t, ST_MARGIN_LEFT);
  return s;
}

std::string styleCss(const Style& s) {
  std::string css;
  if (s.bold) css += "font-weight:bold;";
  if (s.italic) css += "font-style:italic;";
  if (s.align == 3) css += "text-align:center;";
  if (s.align == 4) css += "text-align:justify;";
  if (s.indent.set && s.indent.value != 0) css += "text-indent:" + cssLength(s.indent) + ";";
  if (s.marginTop.set) css += "margin-top:" + cssLength(s.marginTop) + ";";
  if (s.marginBottom.set) css += "margin-bottom:" + cssLength(s.marginBottom) + ";";
  if (s.marginLeft.set && s.marginLeft.value != 0) css += "margin-left:" + cssLength(s.marginLeft) + ";";
  return css;
}

// ---- book model ----

struct Range {
  uint32_t off = 0;
  uint32_t len = 0;
};

std::vector<Range> readRanges(Fb& fb, uint32_t off, uint32_t len) {
  std::vector<Range> out;
  for (uint32_t i = 0; i + 8 <= len && fb.ok(); i += 8) out.push_back({fb.u32(off + i), fb.u32(off + i + 4)});
  return out;
}

struct Chapter {
  std::string title;
  uint32_t dataOff = 0;
  uint32_t blocksOff = 0, blocksLen = 0;
  uint32_t elemIdxOff = 0, elemIdxLen = 0;
  uint32_t blockCount = 0, elemCount = 0;
};

struct TocEntry {
  std::string title;
  std::string href;
  int level = 1;
  int chapter = 0;
  int block = 0;
};

struct Image {
  uint32_t dataOff = 0;  // first pixel byte
  uint32_t dataLen = 0;
  uint16_t width = 0, height = 0;
};

std::string hrefFile(const std::string& href) {
  const size_t hash = href.find('#');
  return hash == std::string::npos ? href : href.substr(0, hash);
}

class Converter {
 public:
  Converter(Fb& fb, ZipWriter& zip) : fb_(fb), zip_(zip) {}

  Result run(const std::string& fallbackTitle) {
    Result r;
    const uint32_t root = fb_.root(0);
    if (fb_.fieldU32(root, 0) != MAGIC || !fb_.ok()) return fail(r, "not an .opub file");
    readMeta();
    readStyles();
    readChapters();
    readToc();
    readImages();
    if (!fb_.ok()) return fail(r, "damaged .opub header");
    if (chapters_.empty()) return fail(r, "no chapters");
    if (title_.empty()) title_ = fallbackTitle;

    zip_.begin("mimetype");
    zip_.write(std::string("application/epub+zip"));
    zip_.end();
    zip_.begin("META-INF/container.xml");
    zip_.write(
        std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<container version=\"1.0\" "
                    "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"OEBPS/"
                    "content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n"));
    zip_.end();

    for (size_t i = 0; i < chapters_.size() && fb_.ok() && zip_.ok(); i++) writeChapter(static_cast<int>(i));
    if (!fb_.ok()) return fail(r, "damaged chapter data");
    if (coverImage_ >= 0) usedImages_.insert(coverImage_);
    for (const int id : usedImages_) writeImage(id);
    writeCss();
    writeNav();
    writeOpf();
    zip_.finish();
    if (!fb_.ok()) return fail(r, "damaged image data");
    if (!zip_.ok()) return fail(r, "write failed (card full?)");
    r.ok = true;
    r.title = title_;
    r.chapters = static_cast<int>(chapters_.size());
    r.images = static_cast<int>(usedImages_.size());
    return r;
  }

 private:
  static Result fail(Result& r, const char* why) {
    r.ok = false;
    r.error = why;
    return r;
  }

  uint32_t section(int k, uint32_t& len) {
    uint32_t off = 0;
    len = 0;
    if (!fb_.fieldRange(fb_.root(0), k, off, len)) return 0;
    return off;
  }

  void readMeta() {
    uint32_t len = 0;
    const uint32_t off = section(SEC_META, len);
    if (!off || !len) return;
    const uint32_t t = fb_.root(off);
    const uint32_t vec = fb_.deref(fb_.field(t, 0));
    if (!vec) return;
    const uint32_t n = fb_.u32(vec);
    for (uint32_t i = 0; i < n && i < 256 && fb_.ok(); i++) {
      const uint32_t e = fb_.deref(vec + 4 + 4 * i);
      const std::string key = fb_.fieldStr(e, 0);
      meta_[key] = fb_.fieldStr(e, 1);
    }
    title_ = meta_["title"];
    author_ = meta_["creator"];
    language_ = meta_["language"];
    bookId_ = meta_["bookId"];
    // "external-cover-*" is a blank placeholder made for books without a cover.
    const std::string cover = meta_["cover-origin"];
    if (!cover.empty()) coverImage_ = atoi(cover.c_str());
  }

  void readStyles() {
    uint32_t len = 0;
    const uint32_t off = section(SEC_STYLES, len);
    for (const Range& r : readRanges(fb_, off, len)) styles_.push_back(readStyle(fb_, fb_.root(r.off)));
  }

  // Style ids are 1-based; 0 means none.
  const Style& style(uint32_t id) const {
    static const Style none;
    return id >= 1 && id <= styles_.size() ? styles_[id - 1] : none;
  }

  void readChapters() {
    uint32_t len = 0;
    const uint32_t off = section(SEC_CHAPTERS, len);
    for (const Range& r : readRanges(fb_, off, len)) {
      const uint32_t t = fb_.root(r.off);
      Chapter c;
      c.title = fb_.fieldStr(t, 1);
      uint32_t dataLen = 0;
      fb_.fieldRange(t, 3, c.dataOff, dataLen);
      fb_.fieldRange(t, 4, c.blocksOff, c.blocksLen);
      fb_.fieldRange(t, 7, c.elemIdxOff, c.elemIdxLen);
      c.blockCount = fb_.fieldU32(t, 8);
      c.elemCount = fb_.fieldU32(t, 9);
      if (!fb_.ok()) return;
      chapters_.push_back(c);
    }
  }

  void readToc() {
    uint32_t len = 0;
    const uint32_t off = section(SEC_TOC, len);
    for (const Range& r : readRanges(fb_, off, len)) {
      const uint32_t t = fb_.root(r.off);
      if (!fb_.field(t, 2)) continue;  // the root entry has no level
      TocEntry e;
      e.title = fb_.fieldStr(t, 0);
      e.href = fb_.fieldStr(t, 1);
      e.level = fb_.fieldU16(t, 2, 1);
      e.chapter = fb_.fieldU16(t, 3);
      e.block = fb_.fieldU16(t, 4);
      if (e.chapter >= static_cast<int>(chapters_.size())) continue;
      toc_.push_back(e);
      const std::string file = hrefFile(e.href);
      if (!file.empty() && !fileChapter_.count(file)) fileChapter_[file] = e.chapter;
      if (!e.href.empty())
        anchorTarget_[e.href] = "c" + std::to_string(e.chapter) + ".xhtml#b" + std::to_string(e.block);
    }
  }

  void readImages() {
    uint32_t len = 0;
    const uint32_t off = section(SEC_IMAGES, len);
    for (const Range& r : readRanges(fb_, off, len)) {
      const uint32_t t = fb_.root(r.off);
      Image img;
      // Field 1 is a table whose first field is the pixel byte vector.
      const uint32_t blob = fb_.deref(fb_.field(t, 1));
      const uint32_t vec = blob ? fb_.deref(fb_.field(blob, 0)) : 0;
      if (vec) {
        img.dataLen = fb_.u32(vec);
        img.dataOff = vec + 4;
      }
      img.width = fb_.fieldU16(t, 4);
      img.height = fb_.fieldU16(t, 5);
      images_.push_back(img);
    }
  }

  // Link target inside the converted book, or empty when unknown.
  std::string linkTarget(const std::string& href) {
    const auto exact = anchorTarget_.find(href);
    if (exact != anchorTarget_.end()) return exact->second;
    const auto file = fileChapter_.find(hrefFile(href));
    if (file != fileChapter_.end()) return "c" + std::to_string(file->second) + ".xhtml";
    return "";
  }

  struct Run {
    uint8_t type = 0;
    std::string text;
    std::string href;
    uint32_t style = 0;
    uint32_t image = 0;
  };

  Run readElement(uint32_t at) {
    Run run;
    const uint32_t t = fb_.root(at);
    run.type = fb_.fieldU8(t, 0);
    const uint32_t c = fb_.deref(fb_.field(t, 1));
    if (!c) return run;
    switch (run.type) {
      case EL_TEXT:
        run.text = fb_.fieldStr(c, 0);
        run.style = fb_.fieldU32(c, 1);
        break;
      case EL_IMAGE:
        run.image = fb_.fieldU32(c, 0);
        run.style = fb_.fieldU32(c, 1);
        break;
      case EL_LINK: {
        run.href = fb_.fieldStr(c, 8);
        const uint32_t textTable = fb_.deref(fb_.field(c, 1));
        if (textTable) {
          run.text = fb_.fieldStr(textTable, 0);
          run.style = fb_.fieldU32(textTable, 1);
        }
        break;
      }
      default:
        break;
    }
    return run;
  }

  std::string inlineText(const Run& run) {
    const Style& s = style(run.style);
    std::string open, close;
    if (s.bold) {
      open += "<b>";
      close = "</b>" + close;
    }
    if (s.italic) {
      open += "<i>";
      close = "</i>" + close;
    }
    return open + escapeXml(run.text) + close;
  }

  void writeChapter(int index) {
    const Chapter& c = chapters_[index];
    zip_.begin("OEBPS/c" + std::to_string(index) + ".xhtml");
    EntryText out(zip_);
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n<html "
           "xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>"
        << escapeXml(c.title)
        << "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n";

    std::vector<Range> elems;
    for (uint32_t i = 0; i < c.elemCount && (i + 1) * 8 <= c.elemIdxLen && fb_.ok(); i++) {
      const uint32_t p = c.dataOff + c.elemIdxOff + i * 8;
      elems.push_back({c.dataOff + fb_.u32(p), fb_.u32(p + 4)});
    }

    for (uint32_t b = 0; b < c.blockCount && (b + 1) * 20 <= c.blocksLen && fb_.ok(); b++) {
      const uint32_t p = c.dataOff + c.blocksOff + b * 20;
      const uint32_t blockStyle = fb_.u32(p + 4);
      const uint32_t first = fb_.u32(p + 12);
      const uint32_t count = fb_.u32(p + 16);

      std::vector<Run> runs;
      for (uint32_t e = first; e < first + count && e < elems.size() && fb_.ok(); e++) {
        runs.push_back(readElement(elems[e].off));
      }

      // Headings: the block (or its first text) is set clearly larger.
      float size = style(blockStyle).sizeEm();
      for (const Run& r : runs) {
        if (r.type == EL_TEXT || r.type == EL_LINK) {
          size = std::max(size, style(r.style).sizeEm());
          break;
        }
      }
      const char* tag = size >= 1.7f ? "h1" : size >= 1.4f ? "h2" : size >= 1.15f ? "h3" : "p";
      out << "<" << tag << " id=\"b" << std::to_string(b) << "\"";
      if (blockStyle >= 1 && blockStyle <= styles_.size()) out << " class=\"s" << std::to_string(blockStyle) << "\"";
      out << ">";
      for (const Run& r : runs) {
        switch (r.type) {
          case EL_TEXT:
            out << inlineText(r);
            break;
          case EL_LINK: {
            const bool external = r.href.rfind("http://", 0) == 0 || r.href.rfind("https://", 0) == 0;
            const std::string target = external ? r.href : linkTarget(r.href);
            if (target.empty()) {
              out << inlineText(r);
            } else {
              out << "<a href=\"" << escapeXml(target) << "\">" << inlineText(r) << "</a>";
            }
            break;
          }
          case EL_BREAK:
            out << "<br/>";
            break;
          case EL_IMAGE:
            if (r.image < images_.size() && images_[r.image].dataLen) {
              usedImages_.insert(static_cast<int>(r.image));
              out << "<img src=\"i" << std::to_string(r.image) << ".png\" alt=\"\"/>";
            }
            break;
          default:
            break;
        }
      }
      out << "</" << tag << ">\n";
    }
    out << "</body></html>\n";
    out.flush();
    zip_.end();
  }

  // Writes the raw pixels as a grayscale PNG with uncompressed deflate blocks.
  void writeImage(int id) {
    if (id < 0 || id >= static_cast<int>(images_.size())) return;
    const Image& img = images_[id];
    if (!img.width || !img.height || !img.dataLen) return;
    const uint64_t pixels = static_cast<uint64_t>(img.width) * img.height;
    int bpp = static_cast<int>((static_cast<uint64_t>(img.dataLen) * 8) / pixels);
    bpp = bpp >= 8 ? 8 : bpp >= 4 ? 4 : bpp >= 2 ? 2 : 1;
    const uint32_t rowBytes = (static_cast<uint32_t>(img.width) * bpp + 7) / 8;
    if (static_cast<uint64_t>(rowBytes) * img.height > img.dataLen) return;

    zip_.begin("OEBPS/i" + std::to_string(id) + ".png");
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    zip_.write(sig, 8);
    uint8_t ihdr[13] = {0};
    put32be(ihdr, img.width);
    put32be(ihdr + 4, img.height);
    ihdr[8] = static_cast<uint8_t>(bpp);
    ihdr[9] = 0;  // grayscale
    chunk("IHDR", ihdr, 13);

    // IDAT: zlib header, stored blocks of filter byte + row, adler32.
    const uint32_t raw = (rowBytes + 1) * img.height;
    const uint32_t blocks = (raw + 65534) / 65535;
    const uint32_t idatLen = 2 + raw + 5 * blocks + 4;
    uint8_t head[8];
    put32be(head, idatLen);
    memcpy(head + 4, "IDAT", 4);
    zip_.write(head, 8);
    uint32_t crc = crc32Update(0, head + 4, 4);
    uint32_t adlerA = 1, adlerB = 0;
    auto emit = [&](const uint8_t* p, size_t n, bool counted) {
      zip_.write(p, n);
      crc = crc32Update(crc, p, n);
      if (!counted) return;
      for (size_t i = 0; i < n; i++) {
        adlerA = (adlerA + p[i]) % 65521;
        adlerB = (adlerB + adlerA) % 65521;
      }
    };
    const uint8_t zhead[2] = {0x78, 0x01};
    emit(zhead, 2, false);

    std::vector<uint8_t> row(rowBytes + 1);
    uint32_t left = raw;         // raw bytes still to emit
    uint32_t blockLeft = 0;      // bytes left in the current stored block
    uint32_t y = 0, rowPos = 0;  // position inside the raw stream
    while (left && fb_.ok()) {
      if (!blockLeft) {
        blockLeft = std::min<uint32_t>(left, 65535);
        const uint8_t bh[5] = {static_cast<uint8_t>(left == blockLeft ? 1 : 0), static_cast<uint8_t>(blockLeft),
                               static_cast<uint8_t>(blockLeft >> 8), static_cast<uint8_t>(~blockLeft),
                               static_cast<uint8_t>(~blockLeft >> 8)};
        emit(bh, 5, false);
      }
      if (rowPos == 0) {
        row[0] = 0;
        fb_.readRaw(img.dataOff + y * rowBytes, row.data() + 1, rowBytes);
      }
      const uint32_t n = std::min(blockLeft, rowBytes + 1 - rowPos);
      emit(row.data() + rowPos, n, true);
      rowPos += n;
      blockLeft -= n;
      left -= n;
      if (rowPos == rowBytes + 1) {
        rowPos = 0;
        y++;
      }
    }
    uint8_t tail[4];
    put32be(tail, (adlerB << 16) | adlerA);
    emit(tail, 4, false);
    uint8_t c[4];
    put32be(c, crc);
    zip_.write(c, 4);
    chunk("IEND", nullptr, 0);
    zip_.end();
  }

  static void put32be(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
  }

  void chunk(const char* type, const uint8_t* data, uint32_t len) {
    uint8_t head[8];
    put32be(head, len);
    memcpy(head + 4, type, 4);
    zip_.write(head, 8);
    if (len) zip_.write(data, len);
    uint32_t crc = crc32Update(0, head + 4, 4);
    if (len) crc = crc32Update(crc, data, len);
    uint8_t c[4];
    put32be(c, crc);
    zip_.write(c, 4);
  }

  void writeCss() {
    zip_.begin("OEBPS/style.css");
    EntryText out(zip_);
    out << "img{max-width:100%;}\n";
    for (size_t i = 0; i < styles_.size(); i++) {
      const std::string css = styleCss(styles_[i]);
      if (!css.empty()) out << ".s" << std::to_string(i + 1) << "{" << css << "}\n";
    }
    out.flush();
    zip_.end();
  }

  void writeNav() {
    zip_.begin("OEBPS/nav.xhtml");
    EntryText out(zip_);
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" "
           "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>"
        << escapeXml(title_) << "</title></head><body><nav epub:type=\"toc\" id=\"toc\"><ol>\n";
    if (toc_.empty()) {
      for (size_t i = 0; i < chapters_.size(); i++) {
        const std::string t = chapters_[i].title.empty() ? std::to_string(i + 1) : chapters_[i].title;
        out << "<li><a href=\"c" << std::to_string(i) << ".xhtml\">" << escapeXml(t) << "</a></li>\n";
      }
    } else {
      // Nest by level; levels start at 1.
      int depth = 1;
      bool open = false;
      for (const TocEntry& e : toc_) {
        const int level = std::max(1, std::min(e.level, depth + 1));
        if (level > depth) {
          out << "<ol>";
          depth = level;
        } else {
          if (open) out << "</li>";
          while (depth > level) {
            out << "</ol></li>";
            depth--;
          }
        }
        out << "\n<li><a href=\"c" << std::to_string(e.chapter) << ".xhtml#b" << std::to_string(e.block) << "\">"
            << escapeXml(e.title.empty() ? std::string("-") : e.title) << "</a>";
        open = true;
      }
      if (open) out << "</li>";
      while (depth > 1) {
        out << "</ol></li>";
        depth--;
      }
    }
    out << "</ol></nav></body></html>\n";
    out.flush();
    zip_.end();
  }

  void writeOpf() {
    zip_.begin("OEBPS/content.opf");
    EntryText out(zip_);
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
           "version=\"3.0\" unique-identifier=\"bookid\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        << "<dc:identifier id=\"bookid\">" << escapeXml(bookId_.empty() ? "opub-" + title_ : bookId_)
        << "</dc:identifier><dc:title>" << escapeXml(title_) << "</dc:title>";
    if (!author_.empty()) out << "<dc:creator>" << escapeXml(author_) << "</dc:creator>";
    out << "<dc:language>" << escapeXml(language_.empty() ? "en" : language_) << "</dc:language>"
        << "<meta property=\"dcterms:modified\">2024-01-01T00:00:00Z</meta>";
    if (coverImage_ >= 0 && usedImages_.count(coverImage_)) {
      out << "<meta name=\"cover\" content=\"i" << std::to_string(coverImage_) << "\"/>";
    }
    out << "</metadata><manifest>\n<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
           "properties=\"nav\"/>\n<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n";
    for (size_t i = 0; i < chapters_.size(); i++) {
      out << "<item id=\"c" << std::to_string(i) << "\" href=\"c" << std::to_string(i)
          << ".xhtml\" media-type=\"application/xhtml+xml\"/>\n";
    }
    for (const int id : usedImages_) {
      out << "<item id=\"i" << std::to_string(id) << "\" href=\"i" << std::to_string(id)
          << ".png\" media-type=\"image/png\"" << (id == coverImage_ ? " properties=\"cover-image\"" : "") << "/>\n";
    }
    out << "</manifest><spine>\n";
    for (size_t i = 0; i < chapters_.size(); i++) out << "<itemref idref=\"c" << std::to_string(i) << "\"/>\n";
    out << "</spine></package>\n";
    out.flush();
    zip_.end();
  }

  Fb& fb_;
  ZipWriter& zip_;
  std::map<std::string, std::string> meta_;
  std::string title_, author_, language_, bookId_;
  int coverImage_ = -1;
  std::vector<Style> styles_;
  std::vector<Chapter> chapters_;
  std::vector<TocEntry> toc_;
  std::vector<Image> images_;
  std::map<std::string, int> fileChapter_;
  std::map<std::string, std::string> anchorTarget_;
  std::set<int> usedImages_;
};

}  // namespace

Result convertToEpub(Source& in, Sink& out, const std::string& fallbackTitle) {
  Result r;
  if (in.size() < 64) {
    r.error = "file too small";
    return r;
  }
  // The block cache is 4 KB; keep it off the caller's stack.
  std::unique_ptr<Fb> fb(new Fb(in));
  ZipWriter zip(out);
  Converter conv(*fb, zip);
  return conv.run(fallbackTitle);
}

bool hasOpubExtension(const char* path) {
  const size_t n = path ? strlen(path) : 0;
  if (n < 5) return false;
  const char* ext = path + n - 5;
  return (ext[0] == '.') && (ext[1] | 0x20) == 'o' && (ext[2] | 0x20) == 'p' && (ext[3] | 0x20) == 'u' &&
         (ext[4] | 0x20) == 'b';
}

}  // namespace opub

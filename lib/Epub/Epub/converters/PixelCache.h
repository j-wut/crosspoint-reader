#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <stdint.h>

#include <cstdlib>
#include <cstring>
#include <string>

// Streaming cache writer for 2-bit pixels (4 levels). Packs 4 pixels per byte,
// MSB first.
//
// The .pxc file is written incrementally in small row bands rather than holding
// the whole decoded image in one heap buffer. A full-page image (e.g. 482x728)
// needs ~88KB packed, which will not fit alongside the ~20KB JPEG decoder on a
// fragmented 380KB heap (free heap is routinely ~55KB on an image page). When
// the cache cannot be written, every render pass re-decodes the JPEG from
// scratch; an anti-aliased image page renders ~14 times (BW + AA restore + two
// grayscale planes x ~6 strips), so a 2s decode becomes a ~30s freeze / watchdog
// reset. Streaming keeps the working set to a single MCU-row band, so caching
// succeeds and the image is decoded exactly once.
//
// Correctness relies on JPEGDEC delivering blocks in raster MCU order (outer
// loop over y, inner over x: see jpeg.inl DecodeJPEG). Consecutive MCU rows map
// to contiguous, non-overlapping destination row ranges, so once a block whose
// top row is Y arrives, every output row < Y is final and is flushed to disk.
struct PixelCache {
  uint8_t* buffer;   // band buffer: (bandRows + 1) rows; last row kept zeroed
  uint8_t* zeroRow;  // points at the spare zeroed row, for gap/clip fill
  int width;
  int height;
  int bytesPerRow;
  int originX;      // config.x - to convert screen coords to cache coords
  int originY;      // config.y
  int bandRows;     // rows held in the band buffer
  int bandStart;    // image-local row index of band buffer row 0
  int flushedRows;  // image-local rows already written to file
  HalFile file;
  std::string cachePathStr;
  bool ok;

  // Line-art check (classifyLineArt): 4-bit gray of the band rows, 2 pixels per byte, plus the two
  // rows above the band, so each finished row can be compared with its neighbours.
  uint8_t* grayBand = nullptr;  // bandRows rows, then a zero row, then the 2 history rows
  int grayBytesPerRow = 0;
  bool classify = false;
  uint32_t flatPixels = 0;
  int classifiedRows = 0;  // finished rows fed to the check
  bool lineArt = false;    // valid after finalize() when the trailer was written

  PixelCache()
      : buffer(nullptr),
        zeroRow(nullptr),
        width(0),
        height(0),
        bytesPerRow(0),
        originX(0),
        originY(0),
        bandRows(0),
        bandStart(0),
        flushedRows(0),
        ok(false) {}

  // Trailer byte after the pixel rows, written when the decode asked for the line-art check.
  static constexpr uint8_t TRAILER_LINE_ART = 'L';
  static constexpr uint8_t TRAILER_GRAY = 'G';
  // A line-art image has at most 1.25% flat gray pixels (gray, and its 3x3 neighbours within one
  // 4-bit step). Plain line figures measure 0-1%, figures with gray fills and photos 1.5% and up.
  static constexpr uint32_t LINE_ART_MAX_FLAT_PER_10000 = 125;
  // The check is skipped (image counts as gray) when its band would need more than this.
  static constexpr size_t LINE_ART_MAX_BYTES = 12 * 1024;
  PixelCache(const PixelCache&) = delete;
  PixelCache& operator=(const PixelCache&) = delete;

  static constexpr int MIN_BAND_ROWS = 16;
  static constexpr size_t MAX_BAND_BYTES = 24 * 1024;  // band working-set ceiling

  // Open the cache file, write the header, and allocate a band buffer big enough
  // to hold the tallest single decode block (maxBlockDstRows output rows).
  bool begin(const std::string& cachePath, int w, int h, int ox, int oy, int maxBlockDstRows,
             bool classifyLineArt = false) {
    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
    bandStart = 0;
    flushedRows = 0;
    ok = false;

    int wantRows = maxBlockDstRows + 2;
    if (wantRows < MIN_BAND_ROWS) wantRows = MIN_BAND_ROWS;
    if (wantRows > h) wantRows = h;

    size_t maxRowsByMem = MAX_BAND_BYTES / (size_t)bytesPerRow;
    if (maxRowsByMem < 1) maxRowsByMem = 1;
    if ((size_t)wantRows > maxRowsByMem) wantRows = (int)maxRowsByMem;

    // A single decode block must fit inside the band, otherwise streaming would
    // drop rows. This only fails for pathological upscales that could not be
    // cached at all; fall back to the no-cache path.
    if (wantRows < maxBlockDstRows) {
      LOG_ERR("IMG", "Cache band too small (%d < %d rows) for %dx%d", wantRows, maxBlockDstRows, w, h);
      return false;
    }
    bandRows = wantRows;

    const size_t bufSize = (size_t)(bandRows + 1) * bytesPerRow;  // +1 spare zero row
    buffer = (uint8_t*)malloc(bufSize);
    if (!buffer) {
      LOG_ERR("IMG", "OOM cache band: %u bytes", (unsigned)bufSize);
      return false;
    }
    memset(buffer, 0, bufSize);
    zeroRow = buffer + (size_t)bandRows * bytesPerRow;

    classify = classifyLineArt;
    flatPixels = 0;
    classifiedRows = 0;
    lineArt = false;
    if (classify) {
      grayBytesPerRow = (w + 1) / 2;
      const size_t graySize = (size_t)(bandRows + 3) * grayBytesPerRow;
      grayBand = graySize <= LINE_ART_MAX_BYTES ? (uint8_t*)calloc(1, graySize) : nullptr;
      if (!grayBand) {
        LOG_DBG("IMG", "Line-art check skipped (%u bytes)", (unsigned)graySize);
        grayBytesPerRow = 0;
      }
    }

    if (!Storage.openFileForWrite("IMG", cachePath, file)) {
      LOG_ERR("IMG", "Failed to open cache file for writing: %s", cachePath.c_str());
      free(buffer);
      buffer = nullptr;
      return false;
    }
    cachePathStr = cachePath;

    uint16_t w16 = (uint16_t)w;
    uint16_t h16 = (uint16_t)h;
    if (file.write(&w16, 2) != 2 || file.write(&h16, 2) != 2) {
      LOG_ERR("IMG", "Failed to write cache header: %s", cachePath.c_str());
      abort();
      return false;
    }

    LOG_DBG("IMG", "Cache stream started: %s (%dx%d, band %d rows)", cachePath.c_str(), w, h, bandRows);
    ok = true;
    return true;
  }

  // Flush every output row below newTopRow (they are final in raster order) and
  // reposition the band to start at newTopRow. Returns false if a write failed,
  // in which case the caller must stop caching for the rest of the decode.
  bool advanceTo(int newTopRow) {
    if (!ok) return false;
    if (newTopRow <= bandStart) return true;
    if (newTopRow > height) newTopRow = height;

    for (int r = bandStart; r < newTopRow; ++r) {
      const int idx = r - bandStart;
      const uint8_t* rowPtr = (idx < bandRows) ? (buffer + (size_t)idx * bytesPerRow) : zeroRow;
      if (file.write(rowPtr, (size_t)bytesPerRow) != (size_t)bytesPerRow) {
        LOG_ERR("IMG", "Cache write error at row %d", r);
        ok = false;
        return false;
      }
      classifyRow(idx);
    }
    flushedRows = newTopRow;
    bandStart = newTopRow;
    memset(buffer, 0, (size_t)bandRows * bytesPerRow);  // fresh band (gaps stay black)
    if (grayBand) memset(grayBand, 0, (size_t)bandRows * grayBytesPerRow);
    return true;
  }

  // Flush the final band and zero-fill any rows never covered (image clipped by
  // the screen), then close the file.
  bool finalize() {
    if (!ok) {
      abort();
      return false;
    }
    for (int r = flushedRows; r < height; ++r) {
      const int idx = r - bandStart;
      const uint8_t* rowPtr = (idx >= 0 && idx < bandRows) ? (buffer + (size_t)idx * bytesPerRow) : zeroRow;
      if (file.write(rowPtr, (size_t)bytesPerRow) != (size_t)bytesPerRow) {
        LOG_ERR("IMG", "Cache write error at row %d", r);
        abort();
        return false;
      }
      classifyRow(idx);
    }
    if (classify) {
      // No line-art check (band too big): the image counts as gray.
      lineArt = grayBand && flatPixels * 10000ULL <= (uint64_t)LINE_ART_MAX_FLAT_PER_10000 * width * height;
      const uint8_t trailer = lineArt ? TRAILER_LINE_ART : TRAILER_GRAY;
      if (file.write(&trailer, 1) != 1) {
        LOG_ERR("IMG", "Cache write error at trailer");
        abort();
        return false;
      }
      LOG_DBG("IMG", "Line-art check: %u flat of %d px -> %s", (unsigned)flatPixels, width * height,
              lineArt ? "line art" : "gray");
    }
    if (!file.close()) {
      LOG_ERR("IMG", "Failed to close cache: %s", cachePathStr.c_str());
      abort();
      return false;
    }
    LOG_DBG("IMG", "Cache written: %s (%dx%d, %d bytes)", cachePathStr.c_str(), width, height,
            4 + bytesPerRow * height);
    ok = false;  // file handed off; nothing left to clean up
    return true;
  }

  // Drop a partial/failed cache so a later decode re-creates it cleanly.
  void abort() {
    if (file.isOpen()) file.close();
    if (!cachePathStr.empty()) {
      Storage.remove(cachePathStr.c_str());
    }
    ok = false;
  }

  ~PixelCache() {
    if (file.isOpen()) {
      // The file is still open, so neither finalize() nor abort() ran, or a
      // mid-stream write failed (advanceTo() cleared ok but left the file open).
      // Drop the partial cache so we leave no corrupt file behind.
      abort();
    }
    if (buffer) {
      free(buffer);
      buffer = nullptr;
    }
    free(grayBand);
    grayBand = nullptr;
  }

 private:
  uint8_t* grayRow(int idx) const {
    return grayBand +
           (size_t)(static_cast<unsigned>(idx) < static_cast<unsigned>(bandRows) ? idx : bandRows) * grayBytesPerRow;
  }
  static int grayAt(const uint8_t* row, int x) { return (x & 1) ? (row[x >> 1] & 0x0F) : (row[x >> 1] >> 4); }

  // Feeds the next finished row (band index idx) to the line-art check. The row above it is now
  // complete with both neighbours, so its pixels are counted. Edge rows and columns are not.
  void classifyRow(int idx) {
    if (!grayBand) return;
    uint8_t* above = grayBand + (size_t)(bandRows + 1) * grayBytesPerRow;
    uint8_t* middle = above + grayBytesPerRow;
    const uint8_t* below = grayRow(idx);
    if (classifiedRows >= 2) {
      for (int x = 1; x + 1 < width; ++x) {
        const int g = grayAt(middle, x);
        if (g < 2 || g > 14) continue;  // white, black and near-black are not fills
        int lo = g, hi = g;
        for (const uint8_t* row : {static_cast<const uint8_t*>(above), static_cast<const uint8_t*>(middle), below}) {
          for (int dx = -1; dx <= 1; ++dx) {
            const int v = grayAt(row, x + dx);
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
          }
        }
        if (hi - lo <= 1) ++flatPixels;
      }
    }
    memcpy(above, middle, grayBytesPerRow);
    memcpy(middle, below, grayBytesPerRow);
    ++classifiedRows;
  }
};

#pragma once

#include <HalStorage.h>

class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true, bool originalThresholds = false);

 public:
  static bool pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop = true, bool originalThresholds = false);
  static bool pngFileToBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  static bool pngFileToBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                         bool crop, bool originalThresholds);
  // Image size from the IHDR chunk; the file position is left at the start.
  static bool pngFileSize(HalFile& pngFile, int& width, int& height);
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                             bool crop = true);
};

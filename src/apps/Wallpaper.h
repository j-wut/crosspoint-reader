#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class GfxRenderer;

// Gallery images, kept in /images like the stock Picco firmware, which saves
// the BOOX Tiles app's images there. Besides BMP, the Gallery and the sleep
// screen take PNG, JPEG and the BOOX .opg images the stock firmware uses.
namespace wallpaper {

constexpr const char* DIR = "/images";
// Sleep image used outside a book (the CrossPoint custom sleep file).
constexpr const char* SLEEP_PATH = "/sleep.bmp";
// Sleep image used while a book is open, like the stock reading wallpaper.
constexpr const char* READING_PATH = "/sleep-reading.bmp";

bool isOpg(std::string_view name);
// Image types the Gallery lists and the sleep screen can draw.
bool isImage(std::string_view name);

// Image files in DIR, sorted by name (full paths).
std::vector<std::string> list();

// A BMP the sleep screen and image viewer can draw for `path`: the file
// itself for a BMP, else a conversion cached in /.crosspoint. False when the
// image cannot be read.
bool screenBmp(const std::string& path, std::string& bmpPath);

// A Gallery preview: a whole 1-bit BMP file held in memory.
struct Thumb {
  std::unique_ptr<uint8_t[]> data;
  size_t size = 0;
  explicit operator bool() const { return size > 0; }
};

// Cache file for a preview of `path` that fits maxWidth x maxHeight. Empty
// when the image cannot be opened.
std::string thumbCachePath(const std::string& path, int maxWidth, int maxHeight);
// Read a cached preview. False when there is none.
bool loadThumb(const std::string& cachePath, Thumb& out);
// Make a preview straight from the image into memory, writing nothing.
bool makeThumb(const std::string& path, int maxWidth, int maxHeight, Thumb& out);
// Write a preview to its cache file.
bool saveThumb(const Thumb& thumb, const std::string& cachePath);
// Draw a preview centred in the box, clipped to it.
void drawThumb(GfxRenderer& renderer, const Thumb& thumb, int x, int y, int width, int height);

// Write an .opg image's first page as a 1- or 2-bit BMP. False when the file
// is not an .opg image this code understands.
bool opgToBmp(const std::string& opgPath, const std::string& bmpPath);

// Copy the image's screen BMP to `destination` (SLEEP_PATH or READING_PATH).
bool setAs(const std::string& path, const char* destination);

// Image picked in the Gallery for each sleep screen, like the stock Picco
// settings (sleepWallpaperPath, readerSleepWallpaperPath, powerOffWallpaperPath).
enum Kind : uint8_t { SLEEP = 0, READING = 1, POWER_OFF = 2 };

// Stored image path for the screen ("" = none).
const char* storedPath(Kind kind);
// Store (or with "" clear) the image for the screen and whether it keeps
// its transparency. Saves settings.
bool setWallpaper(const std::string& path, Kind kind, bool keepAlpha = true);
// Whether the screen's image keeps its transparency.
bool keepsAlpha(Kind kind);
// True when the file is an .opg image with an alpha channel.
bool opgHasAlpha(const std::string& path);

struct DrawResult {
  bool ok = false;
  bool hasAlpha = false;
};
// Draw an .opg image into the frame buffer like the stock firmware: shrunk to
// fit when larger than the screen, centred. The file says whether it has an
// alpha channel: transparent pixels keep what is already on screen, and
// images without alpha clear the screen to white first. With keepAlpha
// false, transparent pixels are white.
DrawResult drawOpg(GfxRenderer& renderer, const std::string& path, bool keepAlpha = true, bool fitScreen = false);

}  // namespace wallpaper

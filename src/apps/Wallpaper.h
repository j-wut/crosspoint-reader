#pragma once

#include <cstdint>
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
// Store (or with "" clear) the image for the screen. Saves settings.
bool setWallpaper(const std::string& path, Kind kind);

struct DrawResult {
  bool ok = false;
  bool hasAlpha = false;
};
// Draw an .opg image into the frame buffer like the stock firmware: shrunk to
// fit when larger than the screen, centred. The file says whether it has an
// alpha channel: transparent pixels keep what is already on screen, and
// images without alpha clear the screen to white first.
DrawResult drawOpg(GfxRenderer& renderer, const std::string& path);

}  // namespace wallpaper

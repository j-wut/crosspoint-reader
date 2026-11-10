#pragma once

#include <Epub/converters/ImageToFramebufferDecoder.h>

#include <string>

class GfxRenderer;

// A Gallery image as the full-screen viewer shows it: shrunk to fit when
// larger than the screen (never enlarged), centred, dithered once. The image
// viewer and the sleep screen both show it from the same screen cache file.
namespace imagescreen {

// Screen cache key for `path` on the renderer's current screen. Empty when
// the file cannot be read.
std::string cacheKey(const std::string& path, const GfxRenderer& renderer);

// Draws the image into the frame buffer and saves its layers under `key`.
// Nothing is shown; the frame buffer is left with the last layer drawn.
bool build(GfxRenderer& renderer, const std::string& path, const std::string& key);

// Where a JPEG of this size goes on the screen.
RenderConfig fitJpeg(const ImageDimensions& dimensions, const GfxRenderer& renderer);

}  // namespace imagescreen

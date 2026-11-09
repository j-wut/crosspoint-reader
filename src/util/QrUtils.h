#pragma once

#include <GfxRenderer.h>

#include <string>

#include "components/themes/BaseTheme.h"

namespace QrUtils {

// Renders a QR code with the given text payload within the specified bounding box.
void drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload);

// Renders a QR code exactly as the stock BOOX firmware does (ESP-IDF
// esp_qrcode, which uses qrcodegen): smallest version up to 10, low error
// correction raised while the data still fits, automatic mask.
void drawStockQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload);

}  // namespace QrUtils

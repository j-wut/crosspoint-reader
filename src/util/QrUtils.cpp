#include "QrUtils.h"

#include <Utf8.h>
#include <qrcode.h>

#include <algorithm>
#include <memory>

#include "Logging.h"

namespace {
void drawModules(const GfxRenderer& renderer, const Rect& bounds, QRCode& qrcode) {
  // Determine the optimal pixel size.
  const int maxDim = std::min(bounds.width, bounds.height);

  int px = maxDim / qrcode.size;
  if (px < 1) px = 1;

  // Calculate centering X and Y
  const int qrDisplaySize = qrcode.size * px;
  const int xOff = bounds.x + (bounds.width - qrDisplaySize) / 2;
  const int yOff = bounds.y + (bounds.height - qrDisplaySize) / 2;

  // Draw the QR Code
  for (uint8_t cy = 0; cy < qrcode.size; cy++) {
    for (uint8_t cx = 0; cx < qrcode.size; cx++) {
      if (qrcode_getModule(&qrcode, cx, cy)) {
        renderer.fillRect(xOff + px * cx, yOff + px * cy, px, px, true);
      }
    }
  }
}
}  // namespace

void QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  // Dynamically calculate the QR code version based on text length
  // Version 4 holds ~114 bytes, Version 10 ~395, Version 20 ~1066, up to 40
  // qrcode.h max version is 40.
  // Formula: approx version = size / 26 + 1 (very rough estimate, better to find best fit)
  size_t len = textPayload.length();

  // Truncate to max QR capacity at a UTF-8 safe boundary to avoid splitting multi-byte sequences
  static constexpr size_t MAX_QR_CAPACITY = 2953;  // Version 40, ECC_LOW, byte mode
  std::string truncated;
  const char* payload = textPayload.c_str();
  if (len > MAX_QR_CAPACITY) {
    len = utf8SafeTruncateBuffer(textPayload.c_str(), static_cast<int>(MAX_QR_CAPACITY));
    truncated = textPayload.substr(0, len);
    payload = truncated.c_str();
  }

  int version = 4;
  if (len > 114) version = 10;
  if (len > 395) version = 20;
  if (len > 1066) version = 30;
  if (len > 2110) version = 40;

  // Make sure we have a large enough buffer on the heap to avoid blowing the stack
  uint32_t bufferSize = qrcode_getBufferSize(version);
  auto qrcodeBytes = std::make_unique<uint8_t[]>(bufferSize);

  QRCode qrcode;
  // Initialize the QR code. We use ECC_LOW for max capacity.
  int8_t res = qrcode_initText(&qrcode, qrcodeBytes.get(), version, ECC_LOW, payload);

  if (res == 0) {
    drawModules(renderer, bounds, qrcode);
  } else {
    // If it fails (e.g. text too large), log an error
    LOG_ERR("QR", "Text too large for QR Code version %d", version);
  }
}

void QrUtils::drawStockQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  // Byte-mode capacity per version 1-10, for ECC LOW, MEDIUM, QUARTILE, HIGH.
  // The QR library writes past its buffer when the text does not fit, so
  // check before encoding.
  static constexpr uint16_t CAPACITY[4][10] = {
      {17, 32, 53, 78, 106, 134, 154, 192, 230, 271},
      {14, 26, 42, 62, 84, 106, 122, 152, 180, 213},
      {11, 20, 32, 46, 60, 74, 86, 108, 130, 151},
      {7, 14, 24, 34, 44, 58, 64, 84, 98, 119},
  };
  constexpr uint8_t MAX_VERSION = 10;
  const size_t len = textPayload.size();
  for (uint8_t version = 1; version <= MAX_VERSION; ++version) {
    if (len > CAPACITY[ECC_LOW][version - 1]) continue;
    // Use the strongest error correction that still fits this version.
    uint8_t ecc = ECC_HIGH;
    while (len > CAPACITY[ecc][version - 1]) --ecc;
    auto buffer = std::make_unique<uint8_t[]>(qrcode_getBufferSize(version));
    QRCode qrcode;
    if (qrcode_initText(&qrcode, buffer.get(), version, ecc, textPayload.c_str()) == 0) {
      drawModules(renderer, bounds, qrcode);
      return;
    }
    break;
  }
  LOG_ERR("QR", "Text too large for QR Code version %d", MAX_VERSION);
}

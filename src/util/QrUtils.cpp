#include "QrUtils.h"

#include <Utf8.h>
#include <qrcode.h>
#include <qrcodegen.h>

#include <algorithm>
#include <memory>

#include "Logging.h"

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
  } else {
    // If it fails (e.g. text too large), log an error
    LOG_ERR("QR", "Text too large for QR Code version %d", version);
  }
}

void QrUtils::drawStockQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  // Same call as ESP-IDF esp_qrcode_generate() with the stock config:
  // versions 1-10, low ECC, automatic mask, boosted ECC. Two 408-byte
  // buffers, freed after drawing.
  constexpr int MAX_VERSION = 10;
  constexpr size_t BUFFER_LEN = qrcodegen_BUFFER_LEN_FOR_VERSION(MAX_VERSION);
  auto qrcode = std::make_unique<uint8_t[]>(BUFFER_LEN);
  auto temp = std::make_unique<uint8_t[]>(BUFFER_LEN);
  if (!qrcodegen_encodeText(textPayload.c_str(), temp.get(), qrcode.get(), qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                            MAX_VERSION, qrcodegen_Mask_AUTO, true)) {
    LOG_ERR("QR", "Text too large for QR Code version %d", MAX_VERSION);
    return;
  }
  const int size = qrcodegen_getSize(qrcode.get());
  const int px = std::max(1, std::min(bounds.width, bounds.height) / size);
  const int xOff = bounds.x + (bounds.width - size * px) / 2;
  const int yOff = bounds.y + (bounds.height - size * px) / 2;
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (qrcodegen_getModule(qrcode.get(), x, y)) renderer.fillRect(xOff + px * x, yOff + px * y, px, px, true);
    }
  }
}

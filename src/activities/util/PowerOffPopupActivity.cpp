#include "PowerOffPopupActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "fontIds.h"

namespace {
// Geometry of the stock popup on a 480x800 screen (BOOX firmware 1.0.5).
constexpr int kCardWidth = 88;
constexpr int kCardHeight = 176;
constexpr int kCardTop = 40;
constexpr int kCardRightGap = 1;
constexpr int kCardBorder = 2;
constexpr int kIconSize = 40;
constexpr int kIconTop = 44;
constexpr int kLabelTop = kIconTop + kIconSize + 8;
constexpr int kLabelSide = 8;

// Stock icon_power_off, 40x40, 1 bpp, MSB first, 0 = ink.
constexpr uint8_t kPowerIcon[kIconSize * kIconSize / 8] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3, 0xFF, 0xFF, 0xFF, 0xCF, 0xC3, 0xF3,
    0xFF, 0xFF, 0x8F, 0xC3, 0xF1, 0xFF, 0xFE, 0x0F, 0xC3, 0xF0, 0x7F, 0xFC, 0x0F, 0xC3, 0xF0, 0x3F, 0xF8, 0x3F, 0xC3,
    0xFC, 0x1F, 0xF8, 0x7F, 0xC3, 0xFE, 0x1F, 0xF0, 0xFF, 0xC3, 0xFF, 0x0F, 0xE1, 0xFF, 0xC3, 0xFF, 0x87, 0xE1, 0xFF,
    0xC3, 0xFF, 0x87, 0xC3, 0xFF, 0xC3, 0xFF, 0xC3, 0xC3, 0xFF, 0xC3, 0xFF, 0xC3, 0xC7, 0xFF, 0xC3, 0xFF, 0xE3, 0x87,
    0xFF, 0xC3, 0xFF, 0xE1, 0x87, 0xFF, 0xC3, 0xFF, 0xE1, 0x8F, 0xFF, 0xC3, 0xFF, 0xE1, 0x8F, 0xFF, 0xC3, 0xFF, 0xF1,
    0x8F, 0xFF, 0xC3, 0xFF, 0xF1, 0x8F, 0xFF, 0xC3, 0xFF, 0xF1, 0x8F, 0xFF, 0xC3, 0xFF, 0xF1, 0x8F, 0xFF, 0xC3, 0xFF,
    0xE1, 0x87, 0xFF, 0xFF, 0xFF, 0xE1, 0x87, 0xFF, 0xFF, 0xFF, 0xE1, 0xC7, 0xFF, 0xFF, 0xFF, 0xE3, 0xC3, 0xFF, 0xFF,
    0xFF, 0xC3, 0xC3, 0xFF, 0xFF, 0xFF, 0xC3, 0xE1, 0xFF, 0xFF, 0xFF, 0x87, 0xE0, 0xFF, 0xFF, 0xFF, 0x07, 0xF0, 0xFF,
    0xFF, 0xFF, 0x0F, 0xF8, 0x7F, 0xFF, 0xFE, 0x1F, 0xF8, 0x1F, 0xFF, 0xF8, 0x1F, 0xFC, 0x0F, 0xFF, 0xF0, 0x3F, 0xFE,
    0x03, 0xFF, 0xC0, 0x7F, 0xFF, 0x80, 0x7E, 0x01, 0xFF, 0xFF, 0xC0, 0x00, 0x03, 0xFF, 0xFF, 0xF0, 0x00, 0x0F, 0xFF,
    0xFF, 0xFE, 0x00, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

struct CardRect {
  int x;
  int y;
  bool contains(const int px, const int py) const {
    return px >= x && px < x + kCardWidth && py >= y && py < y + kCardHeight;
  }
};

// Top-right corner of the current orientation, like the stock layout.
CardRect cardRect(const GfxRenderer& renderer) {
  return {renderer.getScreenWidth() - kCardRightGap - kCardWidth, kCardTop};
}
}  // namespace

void PowerOffPopupActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void PowerOffPopupActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (cardRect(renderer).contains(x, y)) {
      LOG_INF("PWR", "Power Off confirmed");
      activityManager.requestPowerOff();
    }
    finish();
    return;
  }
  for (uint8_t b = 0; b <= static_cast<uint8_t>(MappedInputManager::Button::Power); ++b) {
    if (mappedInput.wasReleased(static_cast<MappedInputManager::Button>(b))) {
      finish();
      return;
    }
  }
}

void PowerOffPopupActivity::render(RenderLock&&) {
  // Drawn over the retained frame of the screen underneath.
  const CardRect card = cardRect(renderer);
  renderer.fillRect(card.x, card.y, kCardWidth, kCardHeight, false);
  renderer.drawRect(card.x, card.y, kCardWidth, kCardHeight, kCardBorder, true);

  const int iconX = card.x + (kCardWidth - kIconSize) / 2;
  const int iconY = card.y + kIconTop;
  constexpr int rowBytes = kIconSize / 8;
  for (int row = 0; row < kIconSize; ++row) {
    for (int col = 0; col < kIconSize; ++col) {
      if (((kPowerIcon[row * rowBytes + col / 8] >> (7 - col % 8)) & 1) == 0) {
        renderer.drawPixel(iconX + col, iconY + row, true);
      }
    }
  }

  const int labelWidth = kCardWidth - 2 * kLabelSide;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const std::vector<std::string> lines = renderer.wrappedText(UI_12_FONT_ID, tr(STR_POWER_OFF), labelWidth, 2);
  int lineY = card.y + kLabelTop;
  for (const auto& line : lines) {
    const int width = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
    renderer.drawText(UI_12_FONT_ID, card.x + kLabelSide + (labelWidth - width) / 2, lineY, line.c_str());
    lineY += lineHeight;
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

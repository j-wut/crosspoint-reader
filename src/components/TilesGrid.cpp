#include "TilesGrid.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>

#include "UITheme.h"

namespace fui = freeink::ui;

namespace tilesgrid {
namespace {
constexpr int16_t TILE_GAP = 12;
constexpr int16_t TILE_ICON = 48;
}  // namespace

void setContentMargin(UiAppHost::UiScreen& screen, const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width) + metrics.contentSidePadding),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x + metrics.contentSidePadding)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
}

void draw(UiAppHost::UiScreen& screen, const Tile* tiles, const int count, const int selected,
          const fui::ActionId action) {
  const auto& theme = screen.theme();
  const int pages = (count + PER_PAGE - 1) / PER_PAGE;
  const int page = selected / PER_PAGE;
  const int16_t labelHeight = screen.target().lineHeight(theme.smallText.font);
  const int16_t pagerHeight = pages > 1 ? labelHeight : 0;
  const fui::Rect body = screen.body();
  const auto tileWidth = static_cast<int16_t>((body.width - TILE_GAP * (COLUMNS - 1)) / COLUMNS);
  // Square-ish tiles; the rows never grow past what fits above the pager.
  const auto tileHeight = static_cast<int16_t>(
      std::min<int>(tileWidth + labelHeight / 2, (body.height - pagerHeight - TILE_GAP * ROWS) / ROWS));
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const fui::Paint paper = fui::Paint::solid(fui::Color::White);
  const int first = page * PER_PAGE;
  const int last = std::min(count, first + PER_PAGE);
  for (int i = first; i < last; ++i) {
    const int slot = i - first;
    const fui::Rect tile{static_cast<int16_t>(body.x + (slot % COLUMNS) * (tileWidth + TILE_GAP)),
                         static_cast<int16_t>(body.y + (slot / COLUMNS) * (tileHeight + TILE_GAP)), tileWidth,
                         tileHeight};
    const bool isSelected = i == selected && !BoardConfig::hasTouch();
    if (isSelected) {
      screen.target().fill(tile, ink, theme.controlRadius);
    } else {
      screen.target().stroke(tile, ink, 2, theme.controlRadius);
    }
    // Labels may wrap to a second line ("Reading Stats").
    auto label = theme.smallText;
    label.align = fui::TextAlign::Center;
    label.inverted = isSelected;
    label.maxLines = 2;
    const char* name = tiles[i].label;
    const auto textWidth = static_cast<int16_t>(tile.width - 2 * theme.spaceSm);
    const int16_t textHeight = fui::measureWrappedText(screen.target(), name, label, textWidth).height;
    const int16_t contentHeight = TILE_ICON + theme.spaceMd + textHeight;
    const fui::Rect iconRect{static_cast<int16_t>(tile.x + (tile.width - TILE_ICON) / 2),
                             static_cast<int16_t>(tile.y + (tile.height - contentHeight) / 2), TILE_ICON, TILE_ICON};
    screen.target().bitmap(iconRect, fui::bitmapFromIcon(*tiles[i].icon), fui::BitmapMode::Center,
                           isSelected ? paper : ink);
    screen.target().text(fui::Rect{static_cast<int16_t>(tile.x + theme.spaceSm),
                                   static_cast<int16_t>(iconRect.bottom() + theme.spaceMd), textWidth, textHeight},
                         name, label);
    screen.frame().hit(tile, action, static_cast<int16_t>(i), fui::InputTouch);
  }
  if (pages > 1) {
    char pager[16];
    snprintf(pager, sizeof(pager), "%d / %d", page + 1, pages);
    auto text = theme.smallText;
    text.align = fui::TextAlign::Center;
    screen.target().text(fui::Rect{body.x, static_cast<int16_t>(body.bottom() - pagerHeight), body.width, pagerHeight},
                         pager, text);
  }
}

}  // namespace tilesgrid

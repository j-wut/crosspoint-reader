#pragma once

#include <Icon.h>

#include "UiAppHost.h"

class GfxRenderer;

// Shared tile grid of the Tiles theme (Apps and File Transfer screens): a
// fixed number of square-ish bordered tiles, each a 48 px icon over a label.
namespace tilesgrid {

constexpr int COLUMNS = 3;
constexpr int ROWS = 3;
constexpr int PER_PAGE = COLUMNS * ROWS;

// Content margins below the header band and above the button hints.
void setContentMargin(UiAppHost::UiScreen& screen, const GfxRenderer& renderer);

struct Tile {
  const char* label;
  const freeink::Icon* icon;  // 48 px
};

// Draws the page of `tiles` that holds `selected`, with a "page / pages" line
// when there is more than one page. Each tile registers a touch hit for
// `action` with its index as value.
void draw(UiAppHost::UiScreen& screen, const Tile* tiles, int count, int selected, freeink::ui::ActionId action);

}  // namespace tilesgrid

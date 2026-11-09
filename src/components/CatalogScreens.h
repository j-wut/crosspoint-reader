#pragma once
#include <initializer_list>

#include "UiAppHost.h"

class GfxRenderer;

// Shared FreeInkUI screen builders for the network catalog activities (the
// OPDS book browser and the SD-plugin catalogs). Both are UiAppHost state
// machines whose states share the same themed chrome: a header band over the
// button hints, centered status/message blocks, a download-progress screen,
// and centered status messages.

// Draw the unified themed header (status band, title, rule, and a back button
// on touch boards) and reserve the button-hint band below the content. A
// trailing icon (e.g. search) becomes a header button; a second icon goes to
// its left.
void catalogScreenHeader(UiAppHost::UiScreen& screen, const GfxRenderer& renderer, const char* title,
                         const freeink::ui::BitmapRef& trailingIcon = {},
                         freeink::ui::ActionId trailingAction = freeink::ui::NO_ACTION,
                         const freeink::ui::BitmapRef& adjacentIcon = {},
                         freeink::ui::ActionId adjacentAction = freeink::ui::NO_ACTION,
                         const freeink::ui::BitmapRef& thirdIcon = {},
                         freeink::ui::ActionId thirdAction = freeink::ui::NO_ACTION, bool thirdFocused = false);

// A third icon button left of a header's two trailing ones (the header itself
// draws at most two). Call right after freeink::ui::header() with the same
// band and props; a long title under the button is painted over. `focused`
// rings it for button navigation.
void headerThirdTrailingButton(UiAppHost::UiScreen& screen, freeink::ui::Rect band,
                               const freeink::ui::HeaderProps& header, const freeink::ui::BitmapRef& icon,
                               freeink::ui::ActionId action, bool focused = false);

// One line of a centered message block; bold marks a heading line.
struct CatalogLine {
  const char* text;
  bool bold = false;
};

// Vertically centered stack of short lines in the remaining content band
// (error screens, completion notices, sign-in hints).
void catalogCenteredBlock(UiAppHost::UiScreen& screen, std::initializer_list<CatalogLine> lines);

// Centered download screen: heading, item title, download progress and, when
// cancelAction is a real action, a Cancel button. A progress bar is shown when
// the total is known (total > 0), otherwise a running byte count.
void catalogDownloadScreen(UiAppHost::UiScreen& screen, const char* status, size_t progress, size_t total,
                           freeink::ui::ActionId cancelAction = freeink::ui::NO_ACTION);

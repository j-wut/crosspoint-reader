#include "TilesHomeUi.h"

#include <BoardConfig.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "UITheme.h"
#include "icons/appIcons.h"
#include "icons/book.h"
#include "util/BookProgress.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId SELECT = 1;
constexpr int16_t COVER_GAP = 14;
constexpr int16_t MAX_COVER_HEIGHT = 330;
constexpr int16_t RING_GAP = 4;
constexpr int16_t RING_WIDTH = 3;
constexpr int16_t DOCK_GAP = 12;
constexpr int16_t DOCK_ICON = 48;
constexpr int16_t PROGRESS_HEIGHT = 6;

const fui::Paint INK = fui::Paint::solid(fui::Color::Black);
const fui::Paint PAPER = fui::Paint::solid(fui::Color::White);
}  // namespace

TilesHomeUi::TilesHomeUi(GfxRenderer& renderer) : UiAppHost(renderer), coverCache(renderer), renderer(renderer) {}

void TilesHomeUi::begin(const std::vector<RecentBook>& recent) {
  books = &recent;
  if (!recent.empty()) coverCache.begin();
  resetUi();
  app.on(SELECT, &TilesHomeUi::onAction, this);
  app.setScreen(&TilesHomeUi::screenFn, this);
  refreshCoverPaths();
  shownBook = 0;
  loadProgress();
}

void TilesHomeUi::loadProgress() {
  progress = shownBook < static_cast<int>(books->size()) ? loadBookProgress((*books)[shownBook].path) : -1;
  if (progress >= 0) snprintf(progressText, sizeof(progressText), "%d%%", progress);
}

void TilesHomeUi::setSelection(const int selection) {
  selected = selection;
  // The info block follows the button cursor while it is on a cover and keeps
  // the last book while the cursor walks the dock.
  if (selection >= 0 && selection < static_cast<int>(books->size()) && selection != shownBook) {
    shownBook = selection;
    loadProgress();
  }
}

void TilesHomeUi::refreshCoverPaths() {
  coverCache.invalidate();
  for (size_t i = 0; i < books->size() && i < coverPaths.size(); ++i) {
    coverPaths[i] =
        thumbHeight > 0 ? UITheme::getCoverThumbPath((*books)[i].coverBmpPath, thumbHeight) : std::string();
    hasThumb[i] = !coverPaths[i].empty() && Storage.exists(coverPaths[i].c_str());
  }
}

int TilesHomeUi::thumbHeightFor() const { return thumbHeight > 0 ? thumbHeight : 300; }

bool TilesHomeUi::takeThumbHeightChanged() { return std::exchange(thumbHeightChanged, false); }

void TilesHomeUi::noteThumbHeight(const int slotHeight) {
  const int height = std::max(1, slotHeight);
  if (thumbHeight != height) {
    thumbHeight = height;
    thumbHeightChanged = true;
    refreshCoverPaths();
  }
}

void TilesHomeUi::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<TilesHomeUi*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

int TilesHomeUi::selectedAction(const MappedInputManager& input) {
  pending = -1;
  const auto touch = routeTouch(input);
  return touch.snap.touchReleased ? pending : -1;
}

void TilesHomeUi::screenFn(UiScreen& screen, void* user) { static_cast<TilesHomeUi*>(user)->draw(screen); }

void TilesHomeUi::draw(UiScreen& screen) {
  coverCache.prepare();
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true);
  const int corner = BoardConfig::ACTIVE.cornerRadius;
  const auto left = static_cast<int16_t>(std::max(safe.x, corner));
  const auto right = static_cast<int16_t>(std::max(renderer.getScreenWidth() - safe.x - safe.width, corner));
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(safe.y), right,
                                                static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height),
                                                left});
  const auto side = static_cast<int16_t>(std::max(0, metrics.contentSidePadding - left));
  screen.insetContent(fui::Insets{0, side, theme.spaceLg, side});

  // Battery band, drawn at the fixed header position every screen uses.
  screen.takeTop(static_cast<int16_t>(metrics.topPadding + metrics.batteryBarHeight), theme.spaceLg);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.batteryBarHeight}, nullptr);

  const fui::Rect body = screen.body();
  const auto dockHeight = static_cast<int16_t>(DOCK_ICON + theme.spaceMd * 3 + screen.target().lineHeight(theme.smallText.font));
  drawDock(screen, screen.takeBottom(dockHeight, theme.spaceLg));

  const auto headingHeight = screen.target().lineHeight(theme.smallText.font);
  if (books->empty()) {
    drawEmpty(screen, screen.body());
    return;
  }
  auto heading = theme.smallText;
  heading.bold = true;
  screen.target().text(screen.takeTop(headingHeight, theme.spaceMd), tr(STR_CONTINUE_READING), heading);
  // The covers take what the title, author and progress lines leave.
  const int16_t infoHeight = static_cast<int16_t>(screen.target().lineHeight(theme.bodyText.font) * 2 +
                                                  screen.target().lineHeight(theme.smallText.font) * 2 +
                                                  theme.spaceSm + theme.spaceMd);
  const auto coverHeight = static_cast<int16_t>(
      std::min<int>(MAX_COVER_HEIGHT, screen.body().height - infoHeight - theme.spaceLg - 2 * (RING_GAP + RING_WIDTH)));
  drawCovers(screen, screen.takeTop(static_cast<int16_t>(coverHeight + 2 * (RING_GAP + RING_WIDTH)), theme.spaceLg));
  drawBookInfo(screen, screen.body());
}

void TilesHomeUi::drawCovers(UiScreen& screen, const fui::Rect rect) {
  // Carousel: the shown book large in the middle, its neighbours cut off at
  // the sides. All three share one thumb size, so each book needs one thumb.
  const auto coverHeight = static_cast<int16_t>(rect.height - 2 * (RING_GAP + RING_WIDTH));
  const auto coverWidth = static_cast<int16_t>(coverHeight * 2 / 3);
  noteThumbHeight(coverHeight);
  const int count = std::min<int>(MAX_BOOKS, books->size());
  const auto top = static_cast<int16_t>(rect.y + RING_GAP + RING_WIDTH);
  const auto centerX = static_cast<int16_t>(rect.x + (rect.width - coverWidth) / 2);
  const auto sideWidth = static_cast<int16_t>(std::max(0, (rect.width - coverWidth) / 2 - COVER_GAP));
  const auto paintCover = [&](const int index, const fui::Rect cover) {
    if (hasThumb[index]) {
      coverCache.paint(cover, index, coverPaths[index]);
      return;
    }
    // No thumbnail yet (or the book has no cover): an outlined card with the title.
    screen.target().fill(cover, PAPER);
    screen.target().stroke(cover, INK, 2, 4);
    auto title = cover.width < 150 ? screen.theme().smallText : screen.theme().bodyText;
    title.bold = true;
    title.align = fui::TextAlign::Center;
    title.maxLines = 6;
    screen.target().text(cover.inset(fui::Insets{24, 10, 24, 10}), (*books)[index].title.c_str(), title);
  };
  if (count > 1 && sideWidth > 0) {
    const int prev = (shownBook + count - 1) % count;
    const int next = (shownBook + 1) % count;
    const fui::Rect leftCover{rect.x, static_cast<int16_t>(top + coverHeight / 10), sideWidth,
                              static_cast<int16_t>(coverHeight * 8 / 10)};
    const fui::Rect rightCover{static_cast<int16_t>(rect.right() - sideWidth), leftCover.y, sideWidth,
                               leftCover.height};
    // With two books both sides show the other one.
    paintCover(prev, leftCover);
    screen.frame().hit(leftCover, SELECT, static_cast<int16_t>(prev), fui::InputTouch);
    if (count > 2 || next != prev) {
      paintCover(next, rightCover);
      screen.frame().hit(rightCover, SELECT, static_cast<int16_t>(next), fui::InputTouch);
    }
  }
  const fui::Rect cover{centerX, top, coverWidth, coverHeight};
  paintCover(shownBook, cover);
  if (selected == shownBook && !BoardConfig::hasTouch()) {
    const fui::Rect ring{static_cast<int16_t>(cover.x - RING_GAP - RING_WIDTH), rect.y,
                         static_cast<int16_t>(cover.width + 2 * (RING_GAP + RING_WIDTH)), rect.height};
    screen.target().stroke(ring, INK, RING_WIDTH, 6);
  }
  screen.frame().hit(cover, SELECT, static_cast<int16_t>(shownBook), fui::InputTouch);
}

void TilesHomeUi::drawBookInfo(UiScreen& screen, fui::Rect rect) {
  const auto& theme = screen.theme();
  const auto& book = (*books)[std::min<int>(shownBook, books->size() - 1)];
  auto title = theme.bodyText;
  title.bold = true;
  title.maxLines = 2;
  const fui::Size titleSize = fui::measureWrappedText(screen.target(), book.title.c_str(), title, rect.width);
  screen.target().text(fui::Rect{rect.x, rect.y, rect.width, titleSize.height}, book.title.c_str(), title);
  auto y = static_cast<int16_t>(rect.y + titleSize.height + theme.spaceSm);
  const int16_t smallHeight = screen.target().lineHeight(theme.smallText.font);
  if (!book.author.empty()) {
    screen.target().text(fui::Rect{rect.x, y, rect.width, smallHeight}, book.author.c_str(), theme.smallText);
    y = static_cast<int16_t>(y + smallHeight + theme.spaceMd);
  }
  if (progress < 0) return;
  // Percent on the right, bar filling the rest of the line.
  const int16_t labelWidth = screen.target().measureText(theme.smallText.font, "100%", theme.smallText).width;
  auto label = theme.smallText;
  label.align = fui::TextAlign::Right;
  screen.target().text(fui::Rect{static_cast<int16_t>(rect.right() - labelWidth), y, labelWidth, smallHeight},
                       progressText, label);
  fui::ProgressBarProps bar;
  bar.value = progress;
  bar.border = INK;
  bar.borderWidth = 1;
  bar.radius = PROGRESS_HEIGHT / 2;
  fui::progressBar(screen.frame(),
                   fui::Rect{rect.x, static_cast<int16_t>(y + (smallHeight - PROGRESS_HEIGHT) / 2),
                             static_cast<int16_t>(rect.width - labelWidth - theme.spaceMd), PROGRESS_HEIGHT},
                   bar);
}

void TilesHomeUi::drawEmpty(UiScreen& screen, const fui::Rect body) {
  const auto& theme = screen.theme();
  auto title = theme.titleText;
  title.bold = true;
  title.align = fui::TextAlign::Center;
  auto message = theme.bodyText;
  message.align = fui::TextAlign::Center;
  constexpr int16_t ICON_SIZE = 32;
  const int16_t titleHeight = screen.target().lineHeight(title.font);
  const int16_t messageHeight = screen.target().lineHeight(message.font);
  const int16_t contentHeight = ICON_SIZE + theme.spaceLg + titleHeight + theme.spaceSm + messageHeight;
  auto y = static_cast<int16_t>(body.y + std::max(0, (body.height - contentHeight) / 2));
  renderer.drawIcon(BookIcon, body.x + (body.width - ICON_SIZE) / 2, y, ICON_SIZE);
  y = static_cast<int16_t>(y + ICON_SIZE + theme.spaceLg);
  screen.target().text(fui::Rect{body.x, y, body.width, titleHeight}, tr(STR_NO_OPEN_BOOK), title);
  y = static_cast<int16_t>(y + titleHeight + theme.spaceSm);
  screen.target().text(fui::Rect{body.x, y, body.width, messageHeight}, tr(STR_TILES_EMPTY_HINT), message);
}

void TilesHomeUi::drawDock(UiScreen& screen, const fui::Rect rect) {
  const auto& theme = screen.theme();
  static const freeink::Icon* const ICONS[DOCK_COUNT] = {&icon_app_books_48, &icon_app_transfer_48, &icon_app_apps_48,
                                                         &icon_app_settings_48};
  const char* const labels[DOCK_COUNT] = {tr(STR_LIBRARY), tr(STR_TRANSFER), tr(STR_APPS), tr(STR_SETTINGS_TITLE)};
  const int base = static_cast<int>(books->size());
  const auto tileWidth = static_cast<int16_t>((rect.width - DOCK_GAP * (DOCK_COUNT - 1)) / DOCK_COUNT);
  const int16_t labelHeight = screen.target().lineHeight(theme.smallText.font);
  for (int i = 0; i < DOCK_COUNT; ++i) {
    const fui::Rect tile{static_cast<int16_t>(rect.x + i * (tileWidth + DOCK_GAP)), rect.y, tileWidth, rect.height};
    const bool isSelected = selected == base + i && !BoardConfig::hasTouch();
    if (isSelected) {
      screen.target().fill(tile, INK, theme.controlRadius);
    } else {
      screen.target().stroke(tile, INK, 2, theme.controlRadius);
    }
    const fui::Rect iconRect{static_cast<int16_t>(tile.x + (tile.width - DOCK_ICON) / 2),
                             static_cast<int16_t>(tile.y + theme.spaceMd + theme.spaceSm), DOCK_ICON, DOCK_ICON};
    screen.target().bitmap(iconRect, fui::bitmapFromIcon(*ICONS[i]), fui::BitmapMode::Center,
                           isSelected ? PAPER : INK);
    auto label = theme.smallText;
    label.align = fui::TextAlign::Center;
    label.inverted = isSelected;
    screen.target().text(fui::Rect{tile.x, static_cast<int16_t>(iconRect.bottom() + theme.spaceMd), tile.width,
                                   labelHeight},
                         labels[i], label);
    screen.frame().hit(tile, SELECT, static_cast<int16_t>(base + i), fui::InputTouch);
  }
}

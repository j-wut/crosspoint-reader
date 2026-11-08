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
#include "util/BookProgress.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId SELECT = 1;
constexpr int MAX_COVER_WIDTH_PCT = 55;
constexpr int16_t RING_GAP = 4;
constexpr int16_t RING_WIDTH = 3;
constexpr int16_t DOT_SIZE = 8;
constexpr int16_t PLUS_CIRCLE = 40;
constexpr int16_t DIVIDER_WIDTH = 1;
constexpr int16_t GRID_ROW_HEIGHT = 84;
constexpr int16_t GRID_ICON = 32;
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
    coverPaths[i] = thumbHeight > 0 ? UITheme::getCoverThumbPath((*books)[i].coverBmpPath, thumbHeight) : std::string();
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
  screen.insetContent(fui::Insets{0, side, theme.spaceMd, side});

  // Battery band, drawn at the fixed header position every screen uses.
  screen.takeTop(static_cast<int16_t>(metrics.topPadding + metrics.batteryBarHeight), theme.spaceLg);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.batteryBarHeight}, nullptr);

  // Two rows of shortcuts at the bottom, a divider, and the book above it.
  // The gap above the grid matches the one below it.
  const auto gridPad = static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height + theme.spaceMd);
  const fui::Rect grid = screen.takeBottom(static_cast<int16_t>(GRID_ROW_HEIGHT * 2), gridPad);
  const fui::Rect body = screen.body();
  const fui::Rect divider{static_cast<int16_t>(body.x - side), static_cast<int16_t>(body.bottom() - DIVIDER_WIDTH),
                          static_cast<int16_t>(body.width + 2 * side), DIVIDER_WIDTH};
  screen.target().fill(divider, INK);
  drawGrid(screen, grid);
  drawBook(screen, fui::Rect{body.x, body.y, body.width,
                             static_cast<int16_t>(body.height - DIVIDER_WIDTH - theme.spaceLg * 2)});
}

void TilesHomeUi::drawBook(UiScreen& screen, const fui::Rect rect) {
  const auto& theme = screen.theme();
  const bool empty = books->empty();
  auto title = theme.bodyText;
  title.bold = true;
  title.align = fui::TextAlign::Center;
  title.maxLines = 1;
  auto subtitle = theme.smallText;
  subtitle.align = fui::TextAlign::Center;
  subtitle.maxLines = 1;
  const int16_t titleHeight = screen.target().lineHeight(title.font);
  const int16_t subtitleHeight = screen.target().lineHeight(subtitle.font);
  const int count = std::min<int>(MAX_BOOKS, books->size());
  const int16_t dotsHeight = count > 1 ? static_cast<int16_t>(DOT_SIZE + theme.spaceMd) : 0;
  // Room for the progress bar is kept even when the book has none, so the
  // cover does not change size between books.
  const int16_t textHeight = static_cast<int16_t>(theme.spaceLg + titleHeight + theme.spaceSm + subtitleHeight +
                                                  theme.spaceMd + PROGRESS_HEIGHT);

  // One centered cover; the ring around it marks the button cursor.
  const int16_t frame = RING_GAP + RING_WIDTH;
  const auto maxCoverHeight = static_cast<int16_t>(rect.height - textHeight - dotsHeight - 2 * frame);
  const auto coverWidth =
      static_cast<int16_t>(std::min<int>(rect.width * MAX_COVER_WIDTH_PCT / 100, maxCoverHeight * 2 / 3));
  const auto coverHeight = static_cast<int16_t>(coverWidth * 3 / 2);
  const fui::Rect cover{static_cast<int16_t>(rect.x + (rect.width - coverWidth) / 2),
                        static_cast<int16_t>(rect.y + frame), coverWidth, coverHeight};
  if (empty) {
    drawPlaceholder(screen, cover);
    // The empty slot opens the bookshelf.
    screen.frame().hit(cover, SELECT, static_cast<int16_t>(Dock::Books), fui::InputTouch);
  } else {
    noteThumbHeight(coverHeight);
    if (hasThumb[shownBook]) {
      coverCache.paint(cover, shownBook, coverPaths[shownBook]);
    } else {
      // No thumbnail yet (or the book has no cover): an outlined card with the title.
      screen.target().fill(cover, PAPER);
      screen.target().stroke(cover, INK, 2, 0);
      auto cardTitle = theme.bodyText;
      cardTitle.bold = true;
      cardTitle.align = fui::TextAlign::Center;
      cardTitle.maxLines = 6;
      screen.target().text(cover.inset(fui::Insets{24, 12, 24, 12}), (*books)[shownBook].title.c_str(), cardTitle);
    }
    if (selected == shownBook && !BoardConfig::hasTouch()) {
      const fui::Rect ring{static_cast<int16_t>(cover.x - frame), static_cast<int16_t>(cover.y - frame),
                           static_cast<int16_t>(cover.width + 2 * frame),
                           static_cast<int16_t>(cover.height + 2 * frame)};
      screen.target().stroke(ring, INK, RING_WIDTH, 6);
    }
    screen.frame().hit(cover, SELECT, static_cast<int16_t>(shownBook), fui::InputTouch);
  }

  auto y = static_cast<int16_t>(cover.bottom() + frame);
  if (count > 1) {
    // One dot per recent book; swipe or the buttons move between them.
    const int16_t dotsWidth = static_cast<int16_t>(count * DOT_SIZE + (count - 1) * DOT_SIZE);
    auto x = static_cast<int16_t>(rect.x + (rect.width - dotsWidth) / 2);
    y = static_cast<int16_t>(y + theme.spaceMd);
    for (int i = 0; i < count; ++i) {
      const fui::Rect dot{x, y, DOT_SIZE, DOT_SIZE};
      if (i == shownBook) {
        screen.target().fill(dot, INK, DOT_SIZE / 2);
      } else {
        screen.target().stroke(dot, INK, 1, DOT_SIZE / 2);
      }
      x = static_cast<int16_t>(x + DOT_SIZE * 2);
    }
    y = static_cast<int16_t>(y + DOT_SIZE);
  }

  y = static_cast<int16_t>(y + theme.spaceLg);
  const char* titleText = empty ? tr(STR_NO_RECENT_BOOKS) : (*books)[shownBook].title.c_str();
  screen.target().text(fui::Rect{rect.x, y, rect.width, titleHeight}, titleText, title);
  y = static_cast<int16_t>(y + titleHeight + theme.spaceSm);
  if (empty) {
    screen.target().text(fui::Rect{rect.x, y, rect.width, subtitleHeight}, tr(STR_TILES_EMPTY_HINT), subtitle);
    return;
  }
  const auto& author = (*books)[shownBook].author;
  char line[96];
  if (progress >= 0 && !author.empty()) {
    snprintf(line, sizeof(line), "%s  ·  %s", progressText, author.c_str());
  } else {
    snprintf(line, sizeof(line), "%s", progress >= 0 ? progressText : author.c_str());
  }
  screen.target().text(fui::Rect{rect.x, y, rect.width, subtitleHeight}, line, subtitle);
  if (progress < 0) return;
  // Reading progress bar, as wide as the cover.
  fui::ProgressBarProps bar;
  bar.value = progress;
  bar.border = INK;
  bar.borderWidth = 1;
  bar.radius = PROGRESS_HEIGHT / 2;
  fui::progressBar(
      screen.frame(),
      fui::Rect{cover.x, static_cast<int16_t>(y + subtitleHeight + theme.spaceMd), cover.width, PROGRESS_HEIGHT}, bar);
}

void TilesHomeUi::drawPlaceholder(UiScreen& screen, const fui::Rect cover) {
  screen.target().fill(cover, fui::Paint::solid(fui::Color::LightGray));
  screen.target().stroke(cover, INK, 2, 0);
  const fui::Rect circle{static_cast<int16_t>(cover.x + (cover.width - PLUS_CIRCLE) / 2),
                         static_cast<int16_t>(cover.y + (cover.height - PLUS_CIRCLE) / 2), PLUS_CIRCLE, PLUS_CIRCLE};
  screen.target().fill(circle, PAPER, PLUS_CIRCLE / 2);
  screen.target().stroke(circle, INK, 2, PLUS_CIRCLE / 2);
  constexpr int16_t ARM = PLUS_CIRCLE / 2;
  constexpr int16_t STROKE = 3;
  const auto cx = static_cast<int16_t>(circle.x + PLUS_CIRCLE / 2);
  const auto cy = static_cast<int16_t>(circle.y + PLUS_CIRCLE / 2);
  screen.target().fill(
      fui::Rect{static_cast<int16_t>(cx - ARM / 2), static_cast<int16_t>(cy - STROKE / 2), ARM, STROKE}, INK);
  screen.target().fill(
      fui::Rect{static_cast<int16_t>(cx - STROKE / 2), static_cast<int16_t>(cy - ARM / 2), STROKE, ARM}, INK);
}

void TilesHomeUi::drawGrid(UiScreen& screen, const fui::Rect rect) {
  const auto& theme = screen.theme();
  static const freeink::Icon* const ICONS[DOCK_COUNT] = {&icon_app_books_32, &icon_app_transfer_32, &icon_app_apps_32,
                                                         &icon_app_settings_32};
  const char* const labels[DOCK_COUNT] = {tr(STR_BOOKSHELF), tr(STR_TRANSFER), tr(STR_APPS), tr(STR_SETTINGS_TITLE)};
  const int base = static_cast<int>(books->size());
  const auto cellWidth = static_cast<int16_t>(rect.width / 2);
  auto label = theme.bodyText;
  label.bold = true;
  label.maxLines = 1;
  const int16_t labelHeight = screen.target().lineHeight(label.font);
  for (int i = 0; i < DOCK_COUNT; ++i) {
    const fui::Rect cell{static_cast<int16_t>(rect.x + (i % 2) * cellWidth),
                         static_cast<int16_t>(rect.y + (i / 2) * GRID_ROW_HEIGHT), cellWidth, GRID_ROW_HEIGHT};
    const bool isSelected = selected == base + i && !BoardConfig::hasTouch();
    if (isSelected) screen.target().fill(cell.inset(fui::Insets{8, 6, 8, 6}), INK, theme.controlRadius);
    // Icon and label start at the same offset in both columns, like a list.
    const auto x = static_cast<int16_t>(cell.x + cell.width / 6);
    const fui::Rect iconRect{x, static_cast<int16_t>(cell.y + (cell.height - GRID_ICON) / 2), GRID_ICON, GRID_ICON};
    screen.target().bitmap(iconRect, fui::bitmapFromIcon(*ICONS[i]), fui::BitmapMode::Center, isSelected ? PAPER : INK);
    label.inverted = isSelected;
    const auto labelX = static_cast<int16_t>(iconRect.right() + theme.spaceMd);
    screen.target().text(fui::Rect{labelX, static_cast<int16_t>(cell.y + (cell.height - labelHeight) / 2),
                                   static_cast<int16_t>(cell.right() - labelX - theme.spaceSm), labelHeight},
                         labels[i], label);
    screen.frame().hit(cell, SELECT, static_cast<int16_t>(base + i), fui::InputTouch);
  }
}

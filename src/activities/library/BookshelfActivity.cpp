#include "BookshelfActivity.h"

#include <BoardConfig.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "util/BookCacheUtils.h"
#include "util/BookProgress.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId CELL = 1;
constexpr fui::ActionId LIST = 2;
constexpr fui::ActionId PAGE = 3;  // value -1 / +1
constexpr int16_t GAP = 14;
constexpr int16_t PROGRESS_HEIGHT = 6;
constexpr int16_t BAR_HEIGHT = 44;
const fui::Paint INK = fui::Paint::solid(fui::Color::Black);
const fui::Paint PAPER = fui::Paint::solid(fui::Color::White);

bool rebuildIndex() {
  library::BuildStats stats;
  if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0)) {
    LOG_ERR("SHELF", "index build failed");
    return false;
  }
  return true;
}

// The book's thumb path without a height, or empty for formats without covers.
std::string thumbBaseFor(const std::string& path) {
  if (FsHelpers::hasReflowableBookExtension(path)) {
    // The constructor only derives cache paths; nothing is parsed.
    auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
    return epub ? epub->getThumbBmpPath() : std::string();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
    return xtc ? xtc->getThumbBmpPath() : std::string();
  }
  return {};
}
}  // namespace

void BookshelfActivity::onEnter() {
  Activity::onEnter();
  {
    // The index walk and the render task's SD font reads must not overlap.
    RenderLock lock(*this);
    if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
    library::LibraryIndexFile index;
    const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
    if (library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
        index.header().metadataEnabled != readMetadata) {
      index.close();
      GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
      rebuildIndex();
    }
  }
  coverCache.begin();
  resetUi();
  app.on(CELL, &BookshelfActivity::onAction, this);
  app.on(LIST, &BookshelfActivity::onAction, this);
  app.on(PAGE, &BookshelfActivity::onAction, this);
  app.setScreen(&BookshelfActivity::screenFn, this);
  loadPage(0);
  requestUpdate();
}

void BookshelfActivity::loadPage(const int newPage) {
  library::LibraryIndexFile index;
  if (!index.open(library::libraryIndexPath())) {
    LOG_ERR("SHELF", "cannot open library index");
    bookCount = 0;
    cellCount = 0;
    return;
  }
  bookCount = index.bookCount();
  const int pages = std::max(1, (bookCount + PER_PAGE - 1) / PER_PAGE);
  page = std::clamp(newPage, 0, pages - 1);
  cellCount = 0;
  for (int slot = 0; slot < PER_PAGE; ++slot) {
    const int row = page * PER_PAGE + slot;
    if (row >= bookCount) break;
    const uint16_t ordinal = index.ordinalForRow(library::SortOrder::RecentDesc, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    Cell& cell = cells[cellCount];
    cell = Cell{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, cell.path)) continue;
    if (!index.readTitle(record, cell.title) || cell.title.empty()) index.readName(record, cell.title);
    ++cellCount;
  }
  // Only one file is open at a time on the card: read the rest after closing.
  index.close();
  for (int i = 0; i < cellCount; ++i) {
    cells[i].progress = loadBookProgress(cells[i].path);
    cells[i].thumbBase = thumbBaseFor(cells[i].path);
  }
  selected = std::min(selected, std::max(0, cellCount - 1));
  refreshThumbs();
}

void BookshelfActivity::refreshThumbs() {
  coverCache.invalidate();
  for (int i = 0; i < cellCount; ++i) {
    Cell& cell = cells[i];
    cell.coverPath = thumbHeight > 0 && !cell.thumbBase.empty()
                         ? UITheme::getCoverThumbPath(cell.thumbBase, thumbHeight)
                         : std::string();
    cell.hasThumb = !cell.coverPath.empty() && Storage.exists(cell.coverPath.c_str());
  }
}

// Makes one missing thumb per call so the page fills in cover by cover.
bool BookshelfActivity::generateMissingThumb() {
  if (thumbHeight <= 0) return false;
  for (int i = 0; i < cellCount; ++i) {
    Cell& cell = cells[i];
    if (cell.hasThumb || cell.thumbTried || cell.thumbBase.empty()) continue;
    cell.thumbTried = true;
    RenderLock lock(*this);
    bool ok = false;
    if (FsHelpers::hasReflowableBookExtension(cell.path)) {
      auto epub = makeUniqueNoThrow<Epub>(cell.path, "/.crosspoint");
      ok = epub && epub->generateThumbBmpFromSource(thumbHeight);
    } else if (FsHelpers::hasXtcExtension(cell.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(cell.path, "/.crosspoint");
      ok = xtc && xtc->load() && xtc->generateThumbBmp(thumbHeight);
    }
    if (!ok) LOG_DBG("SHELF", "no cover for %s", cell.path.c_str());
    cell.hasThumb = ok && Storage.exists(cell.coverPath.c_str());
    coverCache.invalidate(static_cast<size_t>(i));
    return true;
  }
  return false;
}

void BookshelfActivity::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<BookshelfActivity*>(user);
  if (event.action == LIST) {
    self.pending = 1000;
  } else if (event.action == PAGE) {
    self.pending = event.value > 0 ? 1002 : 1001;
  } else {
    self.pending = event.value;
  }
  self.pendingLong = event.longPress;
  self.app.clearTapFlash();
}

void BookshelfActivity::screenFn(UiScreen& screen, void* user) { static_cast<BookshelfActivity*>(user)->draw(screen); }

void BookshelfActivity::select(const int index) {
  const int count = bookCount;
  if (count == 0) return;
  const int flat = (index % count + count) % count;
  const int targetPage = flat / PER_PAGE;
  selected = flat % PER_PAGE;
  if (targetPage != page) loadPage(targetPage);
  requestUpdate();
}

void BookshelfActivity::openCell(const int slot) {
  if (slot < 0 || slot >= cellCount) return;
  onSelectBook(cells[slot].path);
}

void BookshelfActivity::promptDelete(const int slot) {
  if (slot < 0 || slot >= cellCount) return;
  const std::string path = cells[slot].path;
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_ITEM_CONFIRM), cells[slot].title);
  if (!confirmation) {
    LOG_ERR("SHELF", "OOM: delete confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    if (!result.isCancelled) {
      RenderLock lock(*this);
      if (!removeBookFile(path)) LOG_ERR("SHELF", "cannot delete %s", path.c_str());
      if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
      GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
      rebuildIndex();
    }
    loadPage(page);
    resetUi();
    requestUpdate();
  });
}

void BookshelfActivity::loop() {
  if (thumbHeightChanged) {
    thumbHeightChanged = false;
    refreshThumbs();
    requestUpdate();
    return;
  }
  if (generateMissingThumb()) {
    requestUpdate();
    return;
  }

  pending = -1;
  pendingLong = false;
  const auto touch = routeTouch(mappedInput, true);
  if ((touch.snap.touchReleased || pendingLong) && pending >= 0) {
    if (pending == 1000) {
      activityManager.goToLibrary();
    } else if (pending == 1001 || pending == 1002) {
      loadPage(page + (pending == 1002 ? 1 : -1));
      requestUpdate();
    } else if (pendingLong) {
      promptDelete(pending);
    } else {
      openCell(pending);
    }
    return;
  }
  const int pages = std::max(1, (bookCount + PER_PAGE - 1) / PER_PAGE);
  const auto swipe = mappedInput.wasSwipe();
  if ((swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) && page + 1 < pages) {
    loadPage(page + 1);
    requestUpdate();
    return;
  }
  if ((swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) && page > 0) {
    loadPage(page - 1);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome(HomeMenuItem::LIBRARY);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openCell(selected);
    return;
  }
  buttonNavigator.onNext([this] { select(page * PER_PAGE + selected + 1); });
  buttonNavigator.onPrevious([this] { select(page * PER_PAGE + selected - 1); });
}

void BookshelfActivity::draw(UiScreen& screen) {
  coverCache.prepare();
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width) + metrics.contentSidePadding),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
      static_cast<int16_t>(safe.x + metrics.contentSidePadding)});
  const fui::Rect body = screen.body();
  const int16_t smallH = screen.target().lineHeight(theme.smallText.font);
  const int16_t bodyH = screen.target().lineHeight(theme.bodyText.font);

  // Top bar: book count and the List button.
  char countText[24];
  snprintf(countText, sizeof(countText), tr(STR_BOOKSHELF_COUNT), bookCount);
  screen.target().text(fui::Rect{body.x, static_cast<int16_t>(body.y + (BAR_HEIGHT - smallH) / 2), body.width, smallH},
                       countText, theme.smallText);
  auto listText = theme.bodyText;
  listText.align = fui::TextAlign::Center;
  const auto listW = static_cast<int16_t>(
      screen.target().measureText(listText.font, tr(STR_BOOKSHELF_LIST), listText).width + theme.spaceLg * 2);
  const fui::Rect listButton{static_cast<int16_t>(body.right() - listW), static_cast<int16_t>(body.y + 4), listW,
                             static_cast<int16_t>(BAR_HEIGHT - 8)};
  screen.target().stroke(listButton, INK, 2, theme.controlRadius);
  screen.target().text(
      fui::Rect{listButton.x, static_cast<int16_t>(listButton.y + (listButton.height - bodyH) / 2), listW, bodyH},
      tr(STR_BOOKSHELF_LIST), listText);
  screen.frame().hit(listButton, LIST, 0, fui::InputTouch);

  if (bookCount == 0) {
    auto title = theme.titleText;
    title.bold = true;
    title.align = fui::TextAlign::Center;
    const int16_t titleH = screen.target().lineHeight(title.font);
    const auto y = static_cast<int16_t>(body.y + body.height / 3);
    screen.target().text(fui::Rect{body.x, y, body.width, titleH}, tr(STR_BOOKSHELF_EMPTY), title);
    auto hint = theme.bodyText;
    hint.align = fui::TextAlign::Center;
    hint.maxLines = 3;
    screen.target().text(fui::Rect{body.x, static_cast<int16_t>(y + titleH + theme.spaceMd), body.width,
                                   static_cast<int16_t>(bodyH * 3)},
                         tr(STR_BOOKSHELF_EMPTY_HINT), hint);
    return;
  }

  // Pager along the bottom: previous, "page / pages", next.
  const int pages = std::max(1, (bookCount + PER_PAGE - 1) / PER_PAGE);
  const int16_t pagerH = BAR_HEIGHT;
  const auto pagerY = static_cast<int16_t>(body.bottom() - pagerH);
  if (pages > 1) {
    char pagerText[16];
    snprintf(pagerText, sizeof(pagerText), "%d / %d", page + 1, pages);
    auto text = theme.bodyText;
    text.align = fui::TextAlign::Center;
    screen.target().text(fui::Rect{body.x, static_cast<int16_t>(pagerY + (pagerH - bodyH) / 2), body.width, bodyH},
                         pagerText, text);
    const int16_t arrowW = 72;
    const fui::Rect prev{body.x, pagerY, arrowW, pagerH};
    const fui::Rect next{static_cast<int16_t>(body.right() - arrowW), pagerY, arrowW, pagerH};
    auto arrow = theme.titleText;
    arrow.bold = true;
    arrow.align = fui::TextAlign::Center;
    const int16_t arrowH = screen.target().lineHeight(arrow.font);
    if (page > 0) {
      screen.target().stroke(prev, INK, 2, theme.controlRadius);
      screen.target().text(fui::Rect{prev.x, static_cast<int16_t>(prev.y + (pagerH - arrowH) / 2), arrowW, arrowH},
                           "<", arrow);
      screen.frame().hit(prev, PAGE, -1, fui::InputTouch);
    }
    if (page + 1 < pages) {
      screen.target().stroke(next, INK, 2, theme.controlRadius);
      screen.target().text(fui::Rect{next.x, static_cast<int16_t>(next.y + (pagerH - arrowH) / 2), arrowW, arrowH},
                           ">", arrow);
      screen.frame().hit(next, PAGE, 1, fui::InputTouch);
    }
  }

  // Grid between the bars.
  const auto gridTop = static_cast<int16_t>(body.y + BAR_HEIGHT + theme.spaceMd);
  const auto gridH = static_cast<int16_t>(pagerY - theme.spaceMd - gridTop);
  const auto cellW = static_cast<int16_t>((body.width - GAP * (COLUMNS - 1)) / COLUMNS);
  const auto cellH = static_cast<int16_t>((gridH - GAP * (ROWS - 1)) / ROWS);
  for (int slot = 0; slot < cellCount; ++slot) {
    const fui::Rect rect{static_cast<int16_t>(body.x + (slot % COLUMNS) * (cellW + GAP)),
                         static_cast<int16_t>(gridTop + (slot / COLUMNS) * (cellH + GAP)), cellW, cellH};
    drawCell(screen, slot, rect);
  }
}

void BookshelfActivity::drawCell(UiScreen& screen, const int slot, const fui::Rect rect) {
  const auto& theme = screen.theme();
  const Cell& cell = cells[slot];
  auto title = theme.smallText;
  title.maxLines = 2;
  title.align = fui::TextAlign::Center;
  const int16_t smallH = screen.target().lineHeight(title.font);
  // Cover on top; two title lines and the progress bar below it.
  const auto textBand = static_cast<int16_t>(theme.spaceSm + smallH * 2 + theme.spaceSm + PROGRESS_HEIGHT);
  const auto coverH = static_cast<int16_t>(std::min<int>(rect.width * 3 / 2, rect.height - textBand));
  const auto coverW = static_cast<int16_t>(coverH * 2 / 3);
  if (thumbHeight != coverH) {
    // Recorded on the render task; the loop regenerates paths for the new size.
    thumbHeight = coverH;
    thumbHeightChanged = true;
  }
  const fui::Rect cover{static_cast<int16_t>(rect.x + (rect.width - coverW) / 2), rect.y, coverW, coverH};
  if (cell.hasThumb && !thumbHeightChanged) {
    coverCache.paint(cover, static_cast<size_t>(slot), cell.coverPath);
  } else {
    // No cover yet (or the format has none): an outlined card with the title.
    screen.target().fill(cover, PAPER);
    screen.target().stroke(cover, INK, 2, 4);
    auto cardTitle = theme.smallText;
    cardTitle.bold = true;
    cardTitle.align = fui::TextAlign::Center;
    cardTitle.maxLines = 6;
    screen.target().text(cover.inset(fui::Insets{16, 8, 16, 8}), cell.title.c_str(), cardTitle);
  }
  if (slot == selected && !BoardConfig::hasTouch()) {
    screen.target().stroke(cover.inset(fui::Insets{-5, -5, -5, -5}), INK, 3, 6);
  }
  auto y = static_cast<int16_t>(cover.bottom() + theme.spaceSm);
  screen.target().text(fui::Rect{rect.x, y, rect.width, static_cast<int16_t>(smallH * 2)}, cell.title.c_str(), title);
  y = static_cast<int16_t>(y + smallH * 2 + theme.spaceSm);
  if (cell.progress >= 0) {
    fui::ProgressBarProps bar;
    bar.value = cell.progress;
    bar.max = 100;
    bar.border = INK;
    bar.borderWidth = 1;
    bar.radius = 3;
    fui::progressBar(screen.frame(), fui::Rect{cover.x, y, cover.width, PROGRESS_HEIGHT}, bar);
  }
  screen.frame().hit(rect, CELL, static_cast<int16_t>(slot), fui::InputTouch | fui::InputLongPress);
}

void BookshelfActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_BOOKSHELF));
  renderUi();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

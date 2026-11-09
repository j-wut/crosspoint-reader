#include "GalleryActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <vector>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/OptionPopup.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

enum Action : int { VIEW, SET_SLEEP, SET_READING, SET_POWER_OFF, DELETE, ACTION_COUNT };

constexpr fui::ActionId CELL = 1;
constexpr fui::ActionId PAGE = 2;  // value -1 / +1
constexpr int16_t GAP = 16;        // stock grid gap
constexpr int16_t BAR_HEIGHT = 44;
const fui::Paint INK = fui::Paint::solid(fui::Color::Black);
const fui::Paint PAPER = fui::Paint::solid(fui::Color::White);

// Option picker for one image, drawn over the Gallery list. Back cancels.
class GalleryImageActivity final : public Activity {
 public:
  GalleryImageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* title, std::string name,
                       std::vector<const char*> labels)
      : Activity("GalleryImage", renderer, mappedInput),
        title(title),
        name(std::move(name)),
        labels(std::move(labels)) {}

  void onEnter() override {
    Activity::onEnter();
    {
      // Keep the screen under the popup so the next popup or screen starts clean.
      RenderLock lock(*this);
      background = renderer.storeBwBuffer();
    }
    popup.show(title, name.c_str(), labels.data(), static_cast<int>(labels.size()), 0,
               [this](const int action) { chosen = action; });
    requestUpdate();
  }

  void loop() override {
    if (popup.handleInput(mappedInput, [this] { requestUpdate(); }) && popup.isActive()) return;
    if (popup.isActive()) return;
    if (chosen < 0) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
    } else {
      setResult(MenuResult{chosen});
    }
    finish();
  }

  void onExit() override {
    if (background) {
      RenderLock lock(*this);
      renderer.restoreBwBuffer(false);
    }
    Activity::onExit();
  }

  void render(RenderLock&&) override { popup.processRender(renderer, mappedInput); }

 private:
  bool background = false;
  const char* title;
  std::string name;
  std::vector<const char*> labels;
  OptionPopup popup;
  int chosen = -1;
};

}  // namespace

void GalleryActivity::onEnter() {
  Activity::onEnter();
  wallpaper::startLogCapture();
  reload();
  resetUi();
  app.on(CELL, &GalleryActivity::onAction, this);
  app.on(PAGE, &GalleryActivity::onAction, this);
  app.setScreen(&GalleryActivity::screenFn, this);
  loadPage(0);
  requestUpdate();
}

void GalleryActivity::onExit() {
  wallpaper::stopLogCapture();
  Activity::onExit();
}

void GalleryActivity::reload() {
  images = wallpaper::list();
  names.clear();
  names.reserve(images.size());
  for (const auto& path : images) names.push_back(path.substr(path.find_last_of('/') + 1));
  LOG_INF("GAL", "%u images", static_cast<unsigned>(images.size()));
}

int GalleryActivity::itemCount() const { return static_cast<int>(images.size()); }

void GalleryActivity::loadPage(const int newPage) {
  RenderLock lock(*this);
  const int count = itemCount();
  const int pages = std::max(1, (count + PER_PAGE - 1) / PER_PAGE);
  page = std::clamp(newPage, 0, pages - 1);
  cellCount = 0;
  for (int slot = 0; slot < PER_PAGE; ++slot) {
    const int item = page * PER_PAGE + slot;
    if (item >= count) break;
    cells[cellCount++] = Cell{item, {}, {}, false};
  }
  for (int slot = cellCount; slot < PER_PAGE; ++slot) cells[slot] = Cell{};
  previewsReady = cellCount == 0;
  loadingShown = false;
  showRing = false;
  ringDecided = false;
  selected = std::min(selected, std::max(0, cellCount - 1));
}

bool GalleryActivity::allPreviewsCached() const {
  for (int i = 0; i < cellCount; ++i) {
    const std::string cachePath = wallpaper::thumbCachePath(images[cells[i].item], thumbW, thumbH);
    if (cachePath.empty() || !Storage.exists(cachePath.c_str())) return false;
  }
  return true;
}

// Makes every preview on the page before it is shown: from the cache file
// when there is one, else straight from the image into memory.
void GalleryActivity::makePreviews() {
  RenderLock lock(*this);
  const unsigned long start = millis();
  int fromCache = 0;
  int made = 0;
  for (int i = 0; i < cellCount; ++i) {
    Cell& cell = cells[i];
    const std::string& path = images[cell.item];
    const unsigned long t0 = millis();
    cell.cachePath = wallpaper::thumbCachePath(path, thumbW, thumbH);
    const unsigned long t1 = millis();
    if (wallpaper::loadThumb(cell.cachePath, cell.thumb)) {
      ++fromCache;
      LOG_INF("GAL", "preview %s: cache %lu ms (key %lu ms)", path.c_str(), millis() - t1, t1 - t0);
    } else if (wallpaper::makeThumb(path, thumbW, thumbH, cell.thumb)) {
      ++made;
      cell.needsSave = !cell.cachePath.empty();
      LOG_INF("GAL", "preview %s: made %lu ms (key %lu ms)", path.c_str(), millis() - t1, t1 - t0);
    } else {
      cell.thumb = {};
      LOG_ERR("GAL", "no preview for %s (%lu ms)", path.c_str(), millis() - t0);
    }
  }
  previewsReady = true;
  LOG_INF("GAL", "page %d: %d previews (%d cached, %d made) in %lu ms, free heap %u", page + 1, cellCount, fromCache,
          made, millis() - start, static_cast<unsigned>(ESP.getFreeHeap()));
}

// Writes one missing cache file per call, after the page is on screen.
bool GalleryActivity::saveOnePreview() {
  for (int i = 0; i < cellCount; ++i) {
    Cell& cell = cells[i];
    if (!cell.needsSave) continue;
    cell.needsSave = false;
    const unsigned long t0 = millis();
    const bool ok = wallpaper::saveThumb(cell.thumb, cell.cachePath);
    LOG_INF("GAL", "cache write %s: %s %lu ms", images[cell.item].c_str(), ok ? "ok" : "failed", millis() - t0);
    return true;
  }
  return false;
}

void GalleryActivity::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<GalleryActivity*>(user);
  self.pending = event.action == PAGE ? (event.value > 0 ? 1002 : 1001) : event.value;
  self.app.clearTapFlash();
}

void GalleryActivity::screenFn(UiScreen& screen, void* user) { static_cast<GalleryActivity*>(user)->draw(screen); }

void GalleryActivity::select(const int item) {
  const int count = itemCount();
  if (count == 0) return;
  const int flat = (item % count + count) % count;
  selected = flat % PER_PAGE;
  if (flat / PER_PAGE != page) loadPage(flat / PER_PAGE);
  requestUpdate();
}

void GalleryActivity::activate(const int item) {
  if (item >= 0 && item < itemCount()) openImage(item);
}

void GalleryActivity::openImage(const int index) {
  auto menu = makeUniqueNoThrow<GalleryImageActivity>(
      renderer, mappedInput, tr(STR_APP_GALLERY), names[index],
      std::vector<const char*>{tr(STR_OPEN), tr(STR_SET_SLEEP_SCREEN), tr(STR_SET_READING_SLEEP_SCREEN),
                               tr(STR_SET_POWER_OFF_SCREEN), tr(STR_DELETE)});
  if (!menu) {
    LOG_ERR("GAL", "OOM: image menu");
    return;
  }
  startActivityForResult(std::move(menu), [this, index](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* menuResult = std::get_if<MenuResult>(&result.data);
    if (!menuResult) return;
    const int action = menuResult->action;
    const bool setsSleep = action == SET_SLEEP || action == SET_READING || action == SET_POWER_OFF;
    if (setsSleep && wallpaper::opgHasAlpha(images[index])) {
      askKeepAlpha(index, action);
    } else {
      runAction(index, action, true);
    }
  });
}

void GalleryActivity::askKeepAlpha(const int index, const int action) {
  auto ask = makeUniqueNoThrow<GalleryImageActivity>(renderer, mappedInput, tr(STR_KEEP_TRANSPARENCY), names[index],
                                                     std::vector<const char*>{tr(STR_YES), tr(STR_NO)});
  if (!ask) {
    LOG_ERR("GAL", "OOM: transparency choice");
    return;
  }
  startActivityForResult(std::move(ask), [this, index, action](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* choice = std::get_if<MenuResult>(&result.data);
    if (choice) runAction(index, action, choice->action == 0);
  });
}

void GalleryActivity::loop() {
  if (thumbSizeChanged) {
    // The first draw measures the preview size; previews for another size are dropped.
    thumbSizeChanged = false;
    if (previewsReady) {
      loadPage(page);
      requestUpdate();
      return;
    }
  }
  if (!previewsReady && thumbW > 0) {
    // A page whose previews are all cached is shown straight away; otherwise
    // the loading ring goes up first, so the touch shows at once.
    if (!ringDecided) {
      ringDecided = true;
      showRing = !allPreviewsCached();
      if (showRing) {
        requestUpdate();
        return;
      }
    }
    if (!showRing || loadingShown) {
      makePreviews();
      requestUpdate();
      return;
    }
  }
  // After the page is shown: write missing cache files, then the log.
  if (previewsReady && !saveOnePreview()) wallpaper::flushLog();

  pending = -1;
  const auto touch = routeTouch(mappedInput);
  if (touch.snap.touchReleased && pending >= 0) {
    if (pending == 1001 || pending == 1002) {
      loadPage(page + (pending == 1002 ? 1 : -1));
      requestUpdate();
    } else if (pending < cellCount) {
      activate(cells[pending].item);
    }
    return;
  }
  const int pages = std::max(1, (itemCount() + PER_PAGE - 1) / PER_PAGE);
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
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selected < cellCount) activate(cells[selected].item);
    return;
  }
  // Press steps one image; hold jumps a page, as in the lists. Pages count
  // from where the press started, so the first step cannot change the page.
  const int current = page * PER_PAGE + selected;
  buttonNavigator.onNextPress([this, current] {
    holdAnchor = current;
    select(current + 1);
  });
  buttonNavigator.onPreviousPress([this, current] {
    holdAnchor = current;
    select(current - 1);
  });
  buttonNavigator.onNextContinuous([this] {
    holdAnchor = ButtonNavigator::nextPageIndex(holdAnchor, itemCount(), PER_PAGE);
    select(holdAnchor);
  });
  buttonNavigator.onPreviousContinuous([this] {
    holdAnchor = ButtonNavigator::previousPageIndex(holdAnchor, itemCount(), PER_PAGE);
    select(holdAnchor);
  });
}

void GalleryActivity::runAction(const int image, const int action, const bool keepAlpha) {
  if (image < 0 || image >= static_cast<int>(images.size())) return;
  const std::string path = images[image];
  switch (action) {
    case VIEW: {
      // Opened on top of the Gallery, so Back returns to this page.
      auto viewer = makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInput, path);
      if (!viewer) {
        LOG_ERR("GAL", "OOM: image viewer");
        return;
      }
      viewer->setReturnToCaller();
      startActivityForResult(std::move(viewer), [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
    case SET_POWER_OFF: {
      GUI.drawPopup(renderer, tr(STR_SETTING_SLEEP_SCREEN));
      std::string bmp;
      // Check now that the sleep screen will be able to draw it.
      const bool ok = (wallpaper::isOpg(path) || wallpaper::screenBmp(path, bmp)) &&
                      wallpaper::setWallpaper(path, wallpaper::POWER_OFF, keepAlpha);
      LOG_INF("GAL", "power-off image %s: %s", path.c_str(), ok ? "ok" : "failed");
      GUI.drawPopup(renderer, ok ? tr(STR_DONE) : tr(STR_SET_SLEEP_SCREEN_FAILED));
      delay(1000);
      break;
    }
    case SET_SLEEP:
    case SET_READING: {
      GUI.drawPopup(renderer, tr(STR_SETTING_SLEEP_SCREEN));
      const bool reading = action == SET_READING;
      bool ok = wallpaper::setAs(path, reading ? wallpaper::READING_PATH : wallpaper::SLEEP_PATH);
      // The source path lets the sleep screen keep an .opg's transparency.
      ok = ok && wallpaper::setWallpaper(path, reading ? wallpaper::READING : wallpaper::SLEEP, keepAlpha);
      if (ok && !reading && SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM) {
        SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
        SETTINGS.saveToFile();
      }
      LOG_INF("GAL", "%s sleep image %s: %s", reading ? "reading" : "standard", path.c_str(), ok ? "ok" : "failed");
      GUI.drawPopup(renderer, ok ? tr(STR_DONE) : tr(STR_SET_SLEEP_SCREEN_FAILED));
      delay(1000);
      break;
    }
    case DELETE: {
      auto confirmation =
          makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_ITEM_CONFIRM), names[image]);
      if (!confirmation) return;
      startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
        if (result.isCancelled) return;
        if (!Storage.remove(path.c_str())) LOG_ERR("GAL", "Delete failed: %s", path.c_str());
        reload();
        loadPage(page);
        requestUpdate();
      });
      return;
    }
    default:
      return;
  }
  reload();
  loadPage(page);
  requestUpdate();
}

void GalleryActivity::draw(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width) + metrics.contentSidePadding),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x + metrics.contentSidePadding)});
  const fui::Rect body = screen.body();
  const int16_t bodyH = screen.target().lineHeight(theme.bodyText.font);

  auto top = body.y;
  if (images.empty()) {
    auto hint = theme.smallText;
    hint.align = fui::TextAlign::Center;
    hint.maxLines = 4;
    const auto hintH = static_cast<int16_t>(screen.target().lineHeight(hint.font) * 4);
    screen.target().text(fui::Rect{body.x, top, body.width, hintH}, tr(STR_GALLERY_EMPTY_HINT), hint);
    top = static_cast<int16_t>(top + hintH + theme.spaceMd);
  }

  // Pager along the bottom: previous, "page / pages", next.
  const int pages = std::max(1, (itemCount() + PER_PAGE - 1) / PER_PAGE);
  const auto pagerY = static_cast<int16_t>(body.bottom() - BAR_HEIGHT - metrics.verticalSpacing);
  if (pages > 1) {
    char pagerText[16];
    snprintf(pagerText, sizeof(pagerText), "%d / %d", page + 1, pages);
    auto text = theme.bodyText;
    text.align = fui::TextAlign::Center;
    screen.target().text(fui::Rect{body.x, static_cast<int16_t>(pagerY + (BAR_HEIGHT - bodyH) / 2), body.width, bodyH},
                         pagerText, text);
    const int16_t arrowW = 72;
    auto arrow = theme.titleText;
    arrow.bold = false;
    arrow.align = fui::TextAlign::Center;
    const int16_t arrowH = screen.target().lineHeight(arrow.font);
    const auto drawArrow = [&](const fui::Rect r, const char* label, const int16_t value) {
      screen.target().stroke(r, INK, 2, theme.controlRadius);
      screen.target().text(fui::Rect{r.x, static_cast<int16_t>(r.y + (BAR_HEIGHT - arrowH) / 2), arrowW, arrowH}, label,
                           arrow);
      screen.frame().hit(r, PAGE, value, fui::InputTouch);
    };
    if (page > 0) drawArrow(fui::Rect{body.x, pagerY, arrowW, BAR_HEIGHT}, "<", -1);
    if (page + 1 < pages) {
      drawArrow(fui::Rect{static_cast<int16_t>(body.right() - arrowW), pagerY, arrowW, BAR_HEIGHT}, ">", 1);
    }
  }

  const auto gridH = static_cast<int16_t>(pagerY - theme.spaceMd - top);
  const auto cellW = static_cast<int16_t>((body.width - GAP * (COLUMNS - 1)) / COLUMNS);
  const auto cellH = static_cast<int16_t>((gridH - GAP * (ROWS - 1)) / ROWS);
  for (int slot = 0; slot < cellCount; ++slot) {
    const fui::Rect rect{static_cast<int16_t>(body.x + (slot % COLUMNS) * (cellW + GAP)),
                         static_cast<int16_t>(top + (slot / COLUMNS) * (cellH + GAP)), cellW, cellH};
    drawCell(screen, slot, rect);
  }
  if (!previewsReady && showRing && cellCount > 0) {
    // Loading ring in the middle of the grid: a thin track with a thick quarter.
    const int cx = body.x + body.width / 2;
    const int cy = top + gridH / 2;
    constexpr int RADIUS = 36;
    for (const auto& [xDir, yDir] : {std::pair{1, -1}, std::pair{1, 1}, std::pair{-1, 1}, std::pair{-1, -1}}) {
      renderer.drawArc(RADIUS, cx, cy, xDir, yDir, 3, true);
    }
    renderer.drawArc(RADIUS, cx, cy, 1, -1, 9, true);
  }
}

void GalleryActivity::drawCell(UiScreen& screen, const int slot, const fui::Rect rect) {
  const auto& theme = screen.theme();
  const Cell& cell = cells[slot];
  auto name = theme.smallText;
  name.align = fui::TextAlign::Center;
  name.maxLines = 1;
  const int16_t nameH = screen.target().lineHeight(name.font);
  // Preview box in the screen's shape on top, the file name below it.
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int maxBoxH = rect.height - theme.spaceSm - nameH;
  int boxW = rect.width;
  int boxH = boxW * screenH / screenW;
  if (boxH > maxBoxH) {
    boxH = maxBoxH;
    boxW = boxH * screenW / screenH;
  }
  const fui::Rect box{static_cast<int16_t>(rect.x + (rect.width - boxW) / 2), rect.y, static_cast<int16_t>(boxW),
                      static_cast<int16_t>(boxH)};
  constexpr int BORDER = 2;
  const int innerW = boxW - 2 * BORDER;
  const int innerH = boxH - 2 * BORDER;
  if (innerW != thumbW || innerH != thumbH) {
    // Recorded on the render task; the loop remakes previews for the new size.
    thumbW = innerW;
    thumbH = innerH;
    thumbSizeChanged = true;
  }
  if (!previewsReady) return;  // loading: the ring stands in for the grid

  screen.target().fill(box, PAPER);
  if (cell.thumb && !thumbSizeChanged) {
    wallpaper::drawThumb(renderer, cell.thumb, box.x + BORDER, box.y + BORDER, innerW, innerH);
  } else {
    // Preview not made yet, or it failed.
    auto label = theme.smallText;
    label.align = fui::TextAlign::Center;
    screen.target().text(fui::Rect{static_cast<int16_t>(box.x + 8), static_cast<int16_t>(box.y + (boxH - nameH) / 2),
                                   static_cast<int16_t>(boxW - 16), nameH},
                         "?", label);
  }
  screen.target().stroke(box, INK, BORDER, 0);
  if (slot == selected && !BoardConfig::hasTouch()) {
    screen.target().stroke(box.inset(fui::Insets{-5, -5, -5, -5}), INK, 3, 6);
  }
  screen.target().text(fui::Rect{rect.x, static_cast<int16_t>(box.bottom() + theme.spaceSm), rect.width, nameH},
                       names[cell.item].c_str(), name);
  screen.frame().hit(rect, CELL, static_cast<int16_t>(slot), fui::InputTouch);
}

void GalleryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_APP_GALLERY));
  renderUi();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (!previewsReady && !showRing) {
    // This draw only measured the preview size; the page is shown once its
    // previews are made.
    return;
  }
  const unsigned long start = millis();
  renderer.displayBuffer();
  if (previewsReady) {
    LOG_INF("GAL", "page %d shown, refresh %lu ms", page + 1, millis() - start);
  } else {
    // The loop makes the previews once the loading screen is up.
    LOG_INF("GAL", "page %d loading screen, refresh %lu ms", page + 1, millis() - start);
    loadingShown = true;
  }
}

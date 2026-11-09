#include "GalleryActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <array>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "apps/Wallpaper.h"
#include "components/OptionPopup.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

enum Action : int { VIEW, SET_SLEEP, SET_READING, SET_POWER_OFF, DELETE, ACTION_COUNT };

// Option picker for one image, drawn over the Gallery list.
class GalleryImageActivity final : public Activity {
 public:
  GalleryImageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string name)
      : Activity("GalleryImage", renderer, mappedInput), name(std::move(name)) {}

  void onEnter() override {
    Activity::onEnter();
    labels = {tr(STR_OPEN), tr(STR_SET_SLEEP_SCREEN), tr(STR_SET_READING_SLEEP_SCREEN), tr(STR_SET_POWER_OFF_SCREEN),
              tr(STR_DELETE)};
    popup.show(tr(STR_APP_GALLERY), name.c_str(), labels.data(), ACTION_COUNT, 0,
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

  void render(RenderLock&&) override { popup.processRender(renderer, mappedInput); }

 private:
  std::string name;
  std::array<const char*, ACTION_COUNT> labels{};
  OptionPopup popup;
  int chosen = -1;
};

}  // namespace

void GalleryActivity::onEnter() {
  reload();
  UiListActivity::onEnter();
}

void GalleryActivity::reload() {
  images = wallpaper::list();
  names.clear();
  names.reserve(images.size());
  for (const auto& path : images) names.push_back(path.substr(path.find_last_of('/') + 1));
  hasReading = Storage.exists(wallpaper::READING_PATH);
}

const char* GalleryActivity::headerTitle() const { return tr(STR_APP_GALLERY); }

int GalleryActivity::listCount() const { return static_cast<int>(images.size()) + 1 + (hasReading ? 1 : 0); }

void GalleryActivity::activateIndex(const int index) {
  const int count = static_cast<int>(images.size());
  if (index < count) {
    openImage(index);
    return;
  }
  app.clearTapFlash();
  if (index == count) {
    // Random: drop the chosen sleep image so the sleep screen picks from /sleep.
    if (Storage.exists(wallpaper::SLEEP_PATH)) Storage.remove(wallpaper::SLEEP_PATH);
    wallpaper::setWallpaper("", wallpaper::SLEEP);
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
  } else {
    Storage.remove(wallpaper::READING_PATH);
    wallpaper::setWallpaper("", wallpaper::READING);
  }
  GUI.drawPopup(renderer, tr(STR_DONE));
  delay(700);
  reload();
  if (nav.selected >= listCount()) moveSelectionTo(listCount() - 1);
  requestUpdate();
}

void GalleryActivity::openImage(const int index) {
  app.clearTapFlash();
  auto menu = makeUniqueNoThrow<GalleryImageActivity>(renderer, mappedInput, names[index]);
  if (!menu) {
    LOG_ERR("GAL", "OOM: image menu");
    return;
  }
  startActivityForResult(std::move(menu), [this, index](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* menuResult = std::get_if<MenuResult>(&result.data);
    if (menuResult) runAction(index, menuResult->action);
  });
}

void GalleryActivity::runAction(const int image, const int action) {
  if (image < 0 || image >= static_cast<int>(images.size())) return;
  const std::string path = images[image];
  switch (action) {
    case VIEW: {
      std::string bmp;
      GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      // The viewer reads BMP, PNG and JPEG; .opg goes through its converted BMP.
      activityManager.goToReader(wallpaper::isOpg(path) && wallpaper::screenBmp(path, bmp) ? bmp : path);
      return;
    }
    case SET_POWER_OFF: {
      GUI.drawPopup(renderer, tr(STR_SETTING_SLEEP_SCREEN));
      std::string bmp;
      // Check now that the sleep screen will be able to draw it.
      const bool ok = (wallpaper::isOpg(path) || wallpaper::screenBmp(path, bmp)) &&
                      wallpaper::setWallpaper(path, wallpaper::POWER_OFF);
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
      ok = ok && wallpaper::setWallpaper(path, reading ? wallpaper::READING : wallpaper::SLEEP);
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
        if (nav.selected >= listCount()) moveSelectionTo(listCount() - 1);
        requestUpdate();
      });
      return;
    }
    default:
      return;
  }
  reload();
  requestUpdate();
}

void GalleryActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (images.empty()) {
    auto hint = screen.theme().smallText;
    hint.align = fui::TextAlign::Center;
    hint.maxLines = 4;
    const fui::Rect body = screen.body();
    const fui::Rect band = screen.takeTop(static_cast<int16_t>(screen.target().lineHeight(hint.font) * 4),
                                          static_cast<int16_t>(metrics.verticalSpacing));
    screen.target().text(
        fui::Rect{static_cast<int16_t>(body.x + 20), band.y, static_cast<int16_t>(body.width - 40), band.height},
        tr(STR_GALLERY_EMPTY_HINT), hint);
  }

  rows.clear();
  rows.reserve(listCount());
  for (size_t i = 0; i < images.size(); ++i) {
    fui::ListItem row;
    row.label = names[i].c_str();
    row.actionValue = static_cast<int16_t>(i);
    rows.push_back(row);
  }
  fui::ListItem random;
  random.label = tr(STR_GALLERY_RANDOM);
  random.bold = true;
  random.actionValue = static_cast<int16_t>(rows.size());
  rows.push_back(random);
  if (hasReading) {
    fui::ListItem off;
    off.label = tr(STR_GALLERY_READING_OFF);
    off.bold = true;
    off.actionValue = static_cast<int16_t>(rows.size());
    rows.push_back(off);
  }

  fui::ListProps props;
  props.items = rows.data();
  props.count = static_cast<uint16_t>(rows.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/TilesGrid.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/appIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId menuItems[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_NETWORK,
    StrId::STR_CALIBRE_WIRELESS,
    StrId::STR_CREATE_HOTSPOT,
#if FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE,
#endif
};
constexpr StrId menuDescs[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_DESC,
    StrId::STR_CALIBRE_DESC,
    StrId::STR_HOTSPOT_DESC,
#if FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE_DESC,
#endif
};
constexpr UIIcon menuIcons[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    UIIcon::Wifi,
    UIIcon::Library,
    UIIcon::Hotspot,
#if FREEINK_CAP_USB_MSC
    UIIcon::Usb,
#endif
};
// 48 px icons for the Tiles theme grid.
const freeink::Icon* const menuTileIcons[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    &icon_app_wifi_48,
    &icon_app_calibre_48,
    &icon_app_hotspot_48,
#if FREEINK_CAP_USB_MSC
    &icon_app_usb_48,
#endif
};
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput) {
  // Entirely static, so built once here rather than every buildScreen() call.
  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i]);
    item.subtitle = I18N.get(menuDescs[i]);
    item.icon = listIconFor(menuIcons[i], 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(i);
    rowItems_[i] = item;
  }
}

int NetworkModeSelectionActivity::listCount() const { return MENU_ITEM_COUNT; }

const char* NetworkModeSelectionActivity::headerTitle() const { return tr(STR_FILE_TRANSFER); }

void NetworkModeSelectionActivity::activateIndex(const int index) {
  // Selection leaves this screen; a lingering flash would gray an unrelated
  // element on the next render.
  app.clearTapFlash();
  nav.selected = index;

  onModeSelected(static_cast<NetworkMode>(index));
}

void NetworkModeSelectionActivity::navigateButtons() {
  if (!UITheme::hasTilesHome()) {
    UiListActivity::navigateButtons();
    return;
  }
  // The grid has no list viewport to page through: a hold just steps.
  buttonNavigator.onNext([this] { moveSelectionTo(ButtonNavigator::nextIndex(nav.selected, listCount())); });
  buttonNavigator.onPrevious([this] { moveSelectionTo(ButtonNavigator::previousIndex(nav.selected, listCount())); });
}

void NetworkModeSelectionActivity::buildTiles(UiScreen& screen) {
  tilesgrid::setContentMargin(screen, renderer);
  const int count = listCount();
  tilesgrid::Tile tiles[tilesgrid::PER_PAGE];
  for (int i = 0; i < count && i < tilesgrid::PER_PAGE; i++) {
    // Rows added after the fixed ones (BOOX Cloud on the Picco) use the cloud icon.
    tiles[i] = {rowItems_[i].label, i < MENU_ITEM_COUNT ? menuTileIcons[i] : &icon_app_cloud_48};
  }
  tilesgrid::draw(screen, tiles, std::min(count, tilesgrid::PER_PAGE), nav.selected, ACTION_ROW);
}

void NetworkModeSelectionActivity::buildScreen(UiScreen& screen) {
  if (UITheme::hasTilesHome()) {
    buildTiles(screen);
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_ was built once in the constructor and is reused here on every
  // repaint.
  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEM_COUNT);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

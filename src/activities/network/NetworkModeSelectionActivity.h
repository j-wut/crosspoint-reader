#pragma once

#include <BoardConfig.h>

#include "activities/UiListActivity.h"

enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, USB_DRIVE, BOOX_CLOUD, BOOX_BIND };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 *
 * The header stays on GUI.drawHeader for the battery indicator.
 */
class NetworkModeSelectionActivity final : public UiListActivity {
 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

#if FREEINK_CAP_USB_MSC
  static constexpr int MENU_ITEM_COUNT = 4;
#else
  static constexpr int MENU_ITEM_COUNT = 3;
#endif

  void onModeSelected(NetworkMode mode);
  void onCancel();

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override { onCancel(); }
  // The Picco adds BOOX Cloud and BOOX Tiles binding rows after the fixed ones.
  int itemCount_ = MENU_ITEM_COUNT;
  const char* headerTitle() const override;

  // Row storage: entirely static (label/subtitle/icon never change), so it's
  // built once in the constructor instead of every buildScreen() call, into
  // fixed-capacity storage that avoids any heap allocation for the row list.
  freeink::ui::ListItem rowItems_[MENU_ITEM_COUNT + 2]{};
};

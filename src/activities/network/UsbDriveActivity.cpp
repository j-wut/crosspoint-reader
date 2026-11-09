#include "UsbDriveActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

void UsbDriveActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.setScreen(&UsbDriveActivity::driveScreen, this);

  // Show the safety instructions before giving the raw SD card to the USB host.
  requestUpdateAndWait();
  if (!Storage.beginUsbDrive()) {
    LOG_ERR("USB", "Unable to start USB Drive");
    preparing = false;
    startFailed = true;
    state = State::IoError;
    startFailureStartedAt = millis();
    requestUpdate();
    return;
  }

  preparing = false;
  state = State::WaitingForHost;
  hostWaitStartedAt = millis();
  requestUpdate();
}

void UsbDriveActivity::onExit() {
  if (!restartRequested) Storage.endUsbDrive();
  Activity::onExit();
}

void UsbDriveActivity::loop() {
  if (restartRequested) return;
  if (!startFailed) {
    const State nextState = Storage.usbDriveState();
    if (nextState != state) {
      LOG_INF("USB", "USB Drive state %d -> %d (stopping=%d)", static_cast<int>(state), static_cast<int>(nextState),
              stopping);
      state = nextState;
      requestUpdate();
    }
  }

  if (stopping) {
    continueDeviceStop();
    return;
  }

  // A battery-powered USB device may stay mounted after cable removal. End
  // sustained suspend sessions too; host sleep intentionally has the same policy.
  if (state == State::Connected && Storage.usbDriveHostSuspended()) {
    if (!hostSuspendPending) {
      hostSuspendPending = true;
      hostSuspendStartedAt = millis();
    } else if (millis() - hostSuspendStartedAt >= HOST_SUSPEND_TIMEOUT_MS) {
      LOG_INF("USB", "USB Drive host suspend timed out; ending session");
      // endUsbDrive() soft-disconnects before teardown and reboot.
      restartToHome();
      return;
    }
  } else {
    hostSuspendPending = false;
  }

  if (state == State::WaitingForHost && millis() - hostWaitStartedAt >= HOST_WAIT_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive host wait timed out");
    restartToHome();
    return;
  }

  if (startFailed && millis() - startFailureStartedAt >= START_FAILURE_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive startup failure timed out");
    restartToHome();
    return;
  }

  if (!startFailed && state == State::IoError) {
    if (!forcedDisconnectRequested) {
      forcedDisconnectRequested = true;
      forcedDisconnectRequestedAt = millis();
      LOG_ERR("USB", "USB Drive I/O error; disconnecting host");
      if (!Storage.disconnectUsbDriveHost()) {
        LOG_ERR("USB", "Unable to request USB Drive host disconnect");
      }
    } else if (millis() - forcedDisconnectRequestedAt >= FORCED_DISCONNECT_TIMEOUT_MS) {
      LOG_ERR("USB", "USB Drive host disconnect timed out; forcing restart");
      restartToHome();
    }
    return;
  }

  const bool exitPressed = mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Power) || mappedInput.wasHomeGesture();
  if (exitPressed && (state == State::WaitingForHost || startFailed)) {
    LOG_INF("USB", "USB Drive exit pressed before host connected");
    restartToHome();
    return;
  }
  if (exitPressed && state == State::Connected) {
    beginDeviceStop();
    return;
  }

  if (state == State::Ejected || state == State::Disconnected || state == State::Unsupported) {
    restartToHome();
  }
}

void UsbDriveActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_USB_DRIVE));

  renderUi();

  if (!stopping && (state == State::WaitingForHost || state == State::Connected || startFailed)) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

void UsbDriveActivity::driveScreen(UiScreen& screen, void* user) {
  static_cast<UsbDriveActivity*>(user)->buildDriveScreen(screen);
}

void UsbDriveActivity::buildDriveScreen(UiScreen& screen) const {
  const char* message = nullptr;
  const char* detail = nullptr;
  const char* secondaryDetail = nullptr;
  if (preparing) {
    message = tr(STR_USB_DRIVE_PREPARING);
    detail = tr(STR_USB_DRIVE_EJECT_HINT);
  } else if (stopping) {
    message = tr(STR_USB_DRIVE_STOPPING);
  } else {
    switch (state) {
      case State::WaitingForHost:
        message = tr(STR_USB_DRIVE_WAITING);
        break;
      case State::Connected:
        message = tr(STR_USB_DRIVE_CONNECTED);
        detail = tr(STR_USB_DRIVE_CONNECT_DELAY);
        secondaryDetail = tr(STR_USB_DRIVE_EXIT_HINT);
        break;
      case State::IoError:
        message = startFailed ? tr(STR_USB_DRIVE_START_ERROR) : tr(STR_USB_DRIVE_ERROR);
        break;
      case State::Ejected:
      case State::Disconnected:
      case State::Unsupported:
        return;
    }
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), static_cast<int16_t>(metrics.contentSidePadding),
      static_cast<int16_t>(metrics.buttonHintsHeight), static_cast<int16_t>(metrics.contentSidePadding)});

  auto messageStyle = screen.theme().smallText;
  messageStyle.align = fui::TextAlign::Center;
  messageStyle.bold = true;
  messageStyle.maxLines = 2;
  auto detailStyle = screen.theme().smallText;
  detailStyle.align = fui::TextAlign::Center;
  detailStyle.maxLines = 3;

  const fui::Rect body = screen.body();
  const int16_t messageHeight = fui::measureWrappedText(screen.target(), message, messageStyle, body.width).height;
  const int16_t detailHeight =
      detail ? fui::measureWrappedText(screen.target(), detail, detailStyle, body.width).height : 0;
  const int16_t secondaryDetailHeight =
      secondaryDetail ? fui::measureWrappedText(screen.target(), secondaryDetail, detailStyle, body.width).height : 0;
  const int16_t gap = detail ? screen.theme().spaceMd : 0;
  const int16_t secondaryGap = secondaryDetail ? screen.theme().spaceMd : 0;
  const int16_t totalHeight =
      static_cast<int16_t>(messageHeight + gap + detailHeight + secondaryGap + secondaryDetailHeight);
  if (body.height > totalHeight) screen.spacer(static_cast<int16_t>((body.height - totalHeight) / 2));

  screen.target().text(screen.takeTop(messageHeight, gap), message, messageStyle);
  if (detail) {
    screen.target().text(screen.takeTop(detailHeight, secondaryGap), detail, detailStyle);
  }
  if (secondaryDetail) {
    screen.target().text(screen.takeTop(secondaryDetailHeight), secondaryDetail, detailStyle);
  }
}

// Same order as the stock BOOX firmware: report the medium as removed, wait for
// host I/O to go idle (bounded), then disconnect and leave.
void UsbDriveActivity::beginDeviceStop() {
  const unsigned long now = millis();
  const uint32_t lastIo = Storage.usbDriveLastIoMs();
  LOG_INF("USB", "USB Drive stop pressed while connected: busy=%d lastIo=%lu ms ago suspended=%d heap=%u",
          Storage.usbDriveIoBusy(), lastIo ? now - lastIo : 0UL, Storage.usbDriveHostSuspended(),
          static_cast<unsigned>(ESP.getFreeHeap()));
  stopping = true;
  stopStartedAt = now;
  Storage.detachUsbDriveMedia();
  requestUpdate();
}

void UsbDriveActivity::continueDeviceStop() {
  const unsigned long now = millis();
  const unsigned long elapsed = now - stopStartedAt;
  const bool busy = Storage.usbDriveIoBusy();
  const uint32_t lastIo = Storage.usbDriveLastIoMs();
  const unsigned long sinceIo = lastIo ? now - lastIo : elapsed;
  const bool idle = !busy && sinceIo >= STOP_IDLE_MS && elapsed >= STOP_IDLE_MS;
  if (!idle && elapsed < STOP_TIMEOUT_MS) return;
  if (idle) {
    LOG_INF("USB", "USB Drive host I/O idle after %lu ms (last I/O %lu ms ago, state=%d); ending session", elapsed,
            sinceIo, static_cast<int>(state));
  } else {
    LOG_ERR("USB", "USB Drive stop timed out after %lu ms (busy=%d, last I/O %lu ms ago, state=%d); ending anyway",
            elapsed, busy, sinceIo, static_cast<int>(state));
  }
  restartToHome();
}

void UsbDriveActivity::restartToHome() {
  if (restartRequested) return;
  restartRequested = true;
  LOG_INF("USB", "USB Drive ending: state=%d stopping=%d startFailed=%d", static_cast<int>(state), stopping,
          startFailed);
  Storage.endUsbDrive();
  delay(20);
  restartToHomeAfterStorageHandoff();
}

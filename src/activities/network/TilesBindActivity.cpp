#include "TilesBindActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/TilesCloud.h"
#include "util/QrUtils.h"
#include "util/TaskWatchdog.h"

namespace {
// The stock firmware polls for the account link for a few minutes.
constexpr unsigned long LINK_TIMEOUT_MS = 180000;
constexpr unsigned long LINK_RETRY_MS = 3000;
// BLE stays up a moment after binding so the app can read the final status.
constexpr unsigned long BLE_LINGER_MS = 5000;
}  // namespace

void TilesBindActivity::onEnter() {
  Activity::onEnter();
  tilescloud::logLine("=== bind screen opened, heap %u", static_cast<unsigned>(ESP.getFreeHeap()));
  requestUpdate();
}

void TilesBindActivity::onExit() {
  tilesbind::stop();
  tilesbind::flushLog();
  Activity::onExit();
}

TilesBindActivity::Phase TilesBindActivity::phase() const {
  if (bound) return Phase::Bound;
  if (timedOut) return Phase::TimedOut;
  if (!started) return Phase::Starting;
  if (tilesbind::stage() == tilesbind::Stage::Failed) return Phase::Failed;
  if (linkStartedAt != 0) return Phase::Linking;
  return Phase::Waiting;
}

void TilesBindActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
    finish();
    return;
  }
  if (!started) {
    // Paint "Starting" first; starting BLE takes a moment.
    requestUpdateAndWait();
    started = true;
    if (!tilesbind::start(qrUrl)) LOG_ERR("BIND", "BLE start failed");
    tilesbind::flushLog();
    requestUpdate();
    return;
  }
  tilesbind::flushLog();

  const tilesbind::Stage stage = tilesbind::stage();
  if (stage != shownStage) {
    shownStage = stage;
    requestUpdate();
  }

  if (bound) {
    if (boundAt != 0 && millis() - boundAt > BLE_LINGER_MS) {
      tilesbind::stop();
      tilesbind::flushLog();
      boundAt = 0;
    }
    return;
  }
  if (timedOut || WiFi.status() != WL_CONNECTED || tilesbind::ssid().empty()) return;

  if (!wifiSaved) {
    // Keep the network the app sent, as the stock firmware does.
    wifiSaved = true;
    RenderLock lock(*this);
    WIFI_STORE.addCredential(tilesbind::ssid(), tilesbind::passphrase());
    WIFI_STORE.setLastConnectedSsid(tilesbind::ssid());
  }
  if (linkStartedAt == 0) {
    linkStartedAt = millis();
    requestUpdate();
  }
  if (millis() - lastLinkTry < LINK_RETRY_MS) return;
  lastLinkTry = millis();
  resetTaskWatchdogIfSubscribed();
  if (tilescloud::finishBinding()) {
    bound = true;
    boundAt = millis();
    tilescloud::logLine("bind: done after %lu s", (millis() - linkStartedAt) / 1000);
    requestUpdate();
  } else if (millis() - linkStartedAt > LINK_TIMEOUT_MS) {
    timedOut = true;
    tilescloud::logLine("bind: no token after %lu s", LINK_TIMEOUT_MS / 1000);
    tilesbind::stop();
    requestUpdate();
  }
}

void TilesBindActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BIND_TITLE), nullptr);

  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  const Phase p = phase();
  int y = top;
  if (!qrUrl.empty() && (p == Phase::Waiting || p == Phase::Linking)) {
    const int size = std::min(pageWidth - 2 * metrics.contentSidePadding, pageHeight / 2);
    QrUtils::drawQrCode(renderer, Rect{(pageWidth - size) / 2, y, size, size}, qrUrl);
    y += size + metrics.verticalSpacing * 2;
    renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_BIND_SCAN), true);
    y += lineHeight * 2;
  } else {
    y += lineHeight * 3;
  }

  const char* status = nullptr;
  switch (p) {
    case Phase::Starting:
      status = tr(STR_LOADING_POPUP);
      break;
    case Phase::Failed:
      status = tr(STR_BIND_FAILED);
      break;
    case Phase::Bound:
      status = tr(STR_BIND_DONE);
      break;
    case Phase::TimedOut:
      status = tr(STR_BIND_TIMEOUT);
      break;
    case Phase::Linking:
      status = tr(STR_BIND_LINKING);
      break;
    case Phase::Waiting:
      switch (shownStage) {
        case tilesbind::Stage::Connected:
        case tilesbind::Stage::SessionReady:
          status = tr(STR_BIND_CONNECTED);
          break;
        case tilesbind::Stage::WifiConnecting:
        case tilesbind::Stage::WifiConnected:
          status = tr(STR_BIND_WIFI);
          break;
        case tilesbind::Stage::WifiFailed:
          status = tr(STR_BIND_WIFI_FAILED);
          break;
        default:
          status = tr(STR_BIND_WAITING);
          break;
      }
      break;
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, status, true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

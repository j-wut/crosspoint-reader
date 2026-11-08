#include "FortuneActivity.h"

#include <BoardConfig.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <esp_random.h>

#include <iterator>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/appIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId DRAW = 1;
constexpr StrId FORTUNES[] = {
    StrId::STR_FORTUNE_1, StrId::STR_FORTUNE_2,  StrId::STR_FORTUNE_3,  StrId::STR_FORTUNE_4,
    StrId::STR_FORTUNE_5, StrId::STR_FORTUNE_6,  StrId::STR_FORTUNE_7,  StrId::STR_FORTUNE_8,
    StrId::STR_FORTUNE_9, StrId::STR_FORTUNE_10, StrId::STR_FORTUNE_11, StrId::STR_FORTUNE_12,
};
constexpr int FORTUNE_COUNT = static_cast<int>(std::size(FORTUNES));
}  // namespace

void FortuneActivity::onEnter() {
  Activity::onEnter();
  current = static_cast<int>(esp_random() % FORTUNE_COUNT);
  resetUi();
  app.on(DRAW, &FortuneActivity::onAction, this);
  app.setScreen(&FortuneActivity::screenFn, this);
  requestUpdate();
}

void FortuneActivity::drawAgain() {
  // Never the same saying twice in a row.
  current = (current + 1 + static_cast<int>(esp_random() % (FORTUNE_COUNT - 1))) % FORTUNE_COUNT;
  requestUpdate();
}

void FortuneActivity::onAction(const fui::ActionEvent&, void* user) {
  auto& self = *static_cast<FortuneActivity*>(user);
  self.pending = true;
  self.app.clearTapFlash();
}

void FortuneActivity::screenFn(UiScreen& screen, void* user) { static_cast<FortuneActivity*>(user)->draw(screen); }

void FortuneActivity::loop() {
  pending = false;
  const auto touch = routeTouch(mappedInput);
  if ((touch.snap.touchReleased && pending) || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    drawAgain();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
}

void FortuneActivity::draw(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width) + metrics.contentSidePadding),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x + metrics.contentSidePadding)});
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const fui::Rect body = screen.body();
  const fui::Rect card{static_cast<int16_t>(body.x + 20), static_cast<int16_t>(body.y + body.height / 8),
                       static_cast<int16_t>(body.width - 40), static_cast<int16_t>(body.height / 2)};
  screen.target().stroke(card, ink, 3, theme.controlRadius);
  screen.target().stroke(card.inset(fui::Insets{8, 8, 8, 8}), ink, 1, theme.controlRadius);
  constexpr int16_t ICON = 48;
  screen.target().bitmap(
      fui::Rect{static_cast<int16_t>(card.x + (card.width - ICON) / 2), static_cast<int16_t>(card.y + 32), ICON, ICON},
      fui::bitmapFromIcon(icon_app_fortune_48), fui::BitmapMode::Center, ink);
  auto text = theme.titleText;
  text.bold = true;
  text.align = fui::TextAlign::Center;
  text.maxLines = 6;
  const fui::Rect textRect{static_cast<int16_t>(card.x + 28), static_cast<int16_t>(card.y + 32 + ICON + 24),
                           static_cast<int16_t>(card.width - 56), static_cast<int16_t>(card.height - ICON - 88)};
  const char* saying = I18n::getInstance().get(FORTUNES[current]);
  const fui::Size size = fui::measureWrappedText(screen.target(), saying, text, textRect.width);
  screen.target().text(fui::Rect{textRect.x, static_cast<int16_t>(textRect.y + (textRect.height - size.height) / 2),
                                 textRect.width, size.height},
                       saying, text);
  screen.frame().hit(card, DRAW, 0, fui::InputTouch);

  // Buttons have the "Again" hint instead.
  if (!BoardConfig::hasTouch()) return;
  auto hint = theme.smallText;
  hint.align = fui::TextAlign::Center;
  const int16_t hintH = screen.target().lineHeight(hint.font);
  screen.target().text(fui::Rect{body.x, static_cast<int16_t>(card.bottom() + theme.spaceLg * 2), body.width, hintH},
                       tr(STR_FORTUNE_HINT), hint);
}

void FortuneActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_APP_FORTUNE));
  renderUi();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_FORTUNE_AGAIN), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

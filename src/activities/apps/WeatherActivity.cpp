#include "WeatherActivity.h"

#include <BoardConfig.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>

#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/BigDigits.h"
#include "components/UITheme.h"
#include "components/icons/weatherIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId PRESS = 1;
constexpr int16_t BUTTON_HEIGHT = 56;
constexpr int16_t TEMP_HEIGHT = 80;

const freeink::Icon& iconFor(const uint8_t code, const bool large) {
  switch (weather::kindFor(code)) {
    case weather::Kind::Clear:
      return large ? icon_wx_clear_96 : icon_wx_clear_48;
    case weather::Kind::Partly:
      return large ? icon_wx_partly_96 : icon_wx_partly_48;
    case weather::Kind::Cloudy:
      return large ? icon_wx_cloudy_96 : icon_wx_cloudy_48;
    case weather::Kind::Fog:
      return large ? icon_wx_fog_96 : icon_wx_fog_48;
    case weather::Kind::Drizzle:
      return large ? icon_wx_drizzle_96 : icon_wx_drizzle_48;
    case weather::Kind::Rain:
      return large ? icon_wx_rain_96 : icon_wx_rain_48;
    case weather::Kind::Snow:
      return large ? icon_wx_snow_96 : icon_wx_snow_48;
    case weather::Kind::Storm:
      return large ? icon_wx_storm_96 : icon_wx_storm_48;
  }
  return large ? icon_wx_cloudy_96 : icon_wx_cloudy_48;
}

StrId labelFor(const uint8_t code) {
  switch (weather::kindFor(code)) {
    case weather::Kind::Clear:
      return StrId::STR_WEATHER_CLEAR;
    case weather::Kind::Partly:
      return StrId::STR_WEATHER_PARTLY;
    case weather::Kind::Cloudy:
      return StrId::STR_WEATHER_CLOUDY;
    case weather::Kind::Fog:
      return StrId::STR_WEATHER_FOG;
    case weather::Kind::Drizzle:
      return StrId::STR_WEATHER_DRIZZLE;
    case weather::Kind::Rain:
      return StrId::STR_WEATHER_RAIN;
    case weather::Kind::Snow:
      return StrId::STR_WEATHER_SNOW;
    case weather::Kind::Storm:
      return StrId::STR_WEATHER_STORM;
  }
  return StrId::STR_WEATHER_CLOUDY;
}

// Day name for a YYYYMMDD date ("Today" for the first forecast day).
const char* dayName(const int32_t date, const bool first) {
  if (first) return tr(STR_COUNTDOWN_TODAY);
  static constexpr StrId NAMES[] = {StrId::STR_WEEKDAY_SUN, StrId::STR_WEEKDAY_MON, StrId::STR_WEEKDAY_TUE,
                                    StrId::STR_WEEKDAY_WED, StrId::STR_WEEKDAY_THU, StrId::STR_WEEKDAY_FRI,
                                    StrId::STR_WEEKDAY_SAT};
  // Days since 1970-01-01 (a Thursday), Howard Hinnant's algorithm.
  int y = date / 10000;
  const unsigned m = date / 100 % 100;
  const unsigned d = date % 100;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const long days = era * 146097L + static_cast<long>(yoe * 365u + yoe / 4u - yoe / 100u + doy) - 719468L;
  const long weekday = ((days % 7) + 11) % 7;  // 0 = Sunday
  return I18n::getInstance().get(NAMES[weekday]);
}

}  // namespace

void WeatherActivity::onEnter() {
  Activity::onEnter();
  weather::load(forecast);
  status = Status::Idle;
  selected = UPDATE;
  resetUi();
  app.on(PRESS, &WeatherActivity::onAction, this);
  app.setScreen(&WeatherActivity::screenFn, this);
  requestUpdate();
}

void WeatherActivity::onExit() {
  stopWifi();
  Activity::onExit();
}

void WeatherActivity::stopWifi() {
  if (!ownsWifi) return;
  ownsWifi = false;
  // The Picco has PSRAM, so the app turns the radio off in place instead of
  // the heap-clearing reboot the button boards use after Wi-Fi.
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

void WeatherActivity::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<WeatherActivity*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

void WeatherActivity::screenFn(UiScreen& screen, void* user) { static_cast<WeatherActivity*>(user)->draw(screen); }

void WeatherActivity::press(const Button button) {
  selected = button;
  if (button == UPDATE) {
    startUpdate();
    return;
  }
  forecast.fahrenheit = !forecast.fahrenheit;
  if (forecast.valid()) weather::save(forecast);
  requestUpdate();
}

void WeatherActivity::startUpdate() {
  if (WiFi.status() == WL_CONNECTED) {
    status = Status::Updating;
    requestUpdate();
    return;
  }
  ownsWifi = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             stopWifi();
                             status = Status::NoWifi;
                           } else {
                             status = Status::Updating;
                           }
                           requestUpdate();
                         });
}

void WeatherActivity::runUpdate() {
  // A unit picked earlier (or the country default from the last fetch) stays.
  const bool keepUnit = forecast.valid();
  switch (weather::fetch(forecast, keepUnit)) {
    case weather::FetchResult::Ok:
      weather::save(forecast);
      status = Status::Idle;
      break;
    case weather::FetchResult::NoLocation:
      status = Status::NoLocation;
      break;
    case weather::FetchResult::NoForecast:
      status = Status::NoForecast;
      break;
  }
  stopWifi();
  requestUpdate();
}

void WeatherActivity::loop() {
  if (status == Status::Updating) {
    // Paint "Updating..." before the blocking requests.
    requestUpdateAndWait();
    runUpdate();
    return;
  }

  pending = -1;
  const auto touch = routeTouch(mappedInput);
  if (touch.snap.touchReleased && pending >= 0) {
    press(static_cast<Button>(pending));
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    press(static_cast<Button>(selected));
    return;
  }
  buttonNavigator.onNext([this] {
    selected = ButtonNavigator::nextIndex(selected, BUTTON_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selected = ButtonNavigator::previousIndex(selected, BUTTON_COUNT);
    requestUpdate();
  });
}

void WeatherActivity::draw(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width) + metrics.contentSidePadding),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
      static_cast<int16_t>(safe.x + metrics.contentSidePadding)});
  const fui::Rect body = screen.body();

  if (forecast.valid()) {
    drawForecast(screen, body);
  } else if (status == Status::Idle) {
    auto hint = theme.bodyText;
    hint.align = fui::TextAlign::Center;
    hint.maxLines = 4;
    const char* text = tr(STR_WEATHER_EMPTY_HINT);
    const fui::Size size = fui::measureWrappedText(screen.target(), text, hint, body.width);
    screen.target().text(fui::Rect{body.x, static_cast<int16_t>(body.y + body.height / 3), body.width, size.height},
                         text, hint);
  }

  // Status line just above the buttons.
  const char* statusLine = nullptr;
  switch (status) {
    case Status::Idle:
      break;
    case Status::Updating:
      statusLine = tr(STR_WEATHER_UPDATING);
      break;
    case Status::NoWifi:
      statusLine = tr(STR_WEATHER_NO_WIFI);
      break;
    case Status::NoLocation:
      statusLine = tr(STR_WEATHER_NO_LOCATION);
      break;
    case Status::NoForecast:
      statusLine = tr(STR_WEATHER_NO_FORECAST);
      break;
  }
  if (statusLine) {
    auto line = theme.bodyText;
    line.bold = true;
    line.align = fui::TextAlign::Center;
    const int16_t lh = screen.target().lineHeight(line.font);
    screen.target().text(
        fui::Rect{body.x, static_cast<int16_t>(body.bottom() - BUTTON_HEIGHT - theme.spaceLg * 3 - lh), body.width, lh},
        statusLine, line);
  }
  drawButtons(screen, body);
}

void WeatherActivity::drawForecast(UiScreen& screen, const fui::Rect body) {
  const auto& theme = screen.theme();
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const bool f = forecast.fahrenheit;
  auto y = static_cast<int16_t>(body.y + theme.spaceLg);

  // Place and when the forecast was fetched.
  auto title = theme.titleText;
  title.bold = true;
  title.align = fui::TextAlign::Center;
  const int16_t titleH = screen.target().lineHeight(title.font);
  if (!forecast.place.empty()) {
    screen.target().text(fui::Rect{body.x, y, body.width, titleH}, forecast.place.c_str(), title);
    y = static_cast<int16_t>(y + titleH + theme.spaceSm);
  }
  auto small = theme.smallText;
  small.align = fui::TextAlign::Center;
  const int16_t smallH = screen.target().lineHeight(small.font);
  if (forecast.fetchedAt > 0) {
    const auto when = static_cast<time_t>(forecast.fetchedAt);
    struct tm local;
    localtime_r(&when, &local);
    char stamp[32];
    strftime(stamp, sizeof(stamp), SETTINGS.clockFormat == 1 ? "%b %d, %I:%M %p" : "%b %d, %H:%M", &local);
    char line[64];
    snprintf(line, sizeof(line), tr(STR_WEATHER_UPDATED), stamp);
    screen.target().text(fui::Rect{body.x, y, body.width, smallH}, line, small);
  }
  y = static_cast<int16_t>(y + smallH + theme.spaceLg * 2);

  // Big icon and temperature side by side.
  constexpr int16_t ICON = 96;
  char temp[8];
  snprintf(temp, sizeof(temp), "%d", weather::toDisplay(forecast.tempC, f));
  const int16_t digitsW = bigdigits::width(temp, TEMP_HEIGHT);
  auto unitText = theme.titleText;
  unitText.bold = true;
  const int16_t ring = 14;
  const int16_t unitW = static_cast<int16_t>(ring + 4 + screen.target().measureText(unitText.font, "F", unitText).width + 4);
  const int16_t gap = theme.spaceLg * 2;
  const auto rowW = static_cast<int16_t>(ICON + gap + digitsW + 8 + unitW);
  auto x = static_cast<int16_t>(body.x + (body.width - rowW) / 2);
  screen.target().bitmap(fui::Rect{x, y, ICON, ICON}, fui::bitmapFromIcon(iconFor(forecast.code, true)),
                         fui::BitmapMode::Center, ink);
  x = static_cast<int16_t>(x + ICON + gap);
  const auto digitsY = static_cast<int16_t>(y + (ICON - TEMP_HEIGHT) / 2);
  bigdigits::draw(screen.target(), x, digitsY, temp, TEMP_HEIGHT);
  x = static_cast<int16_t>(x + digitsW + 8);
  screen.target().stroke(fui::Rect{x, digitsY, ring, ring}, ink, 3, ring / 2);
  screen.target().text(fui::Rect{static_cast<int16_t>(x + ring + 4), digitsY, unitW, titleH}, f ? "F" : "C", unitText);
  y = static_cast<int16_t>(y + ICON + theme.spaceLg);

  // Condition, humidity and wind.
  screen.target().text(fui::Rect{body.x, y, body.width, titleH}, I18n::getInstance().get(labelFor(forecast.code)),
                       title);
  y = static_cast<int16_t>(y + titleH + theme.spaceSm);
  char details[64];
  const int wind = static_cast<int>(f ? forecast.windKmh * 0.621371f + 0.5f : forecast.windKmh + 0.5f);
  snprintf(details, sizeof(details), tr(STR_WEATHER_DETAILS), static_cast<unsigned>(forecast.humidity), wind,
           f ? "mph" : "km/h");
  auto body2 = theme.bodyText;
  body2.align = fui::TextAlign::Center;
  const int16_t bodyH = screen.target().lineHeight(body2.font);
  screen.target().text(fui::Rect{body.x, y, body.width, bodyH}, details, body2);
  y = static_cast<int16_t>(y + bodyH + theme.spaceLg * 2);

  // One card per forecast day.
  const int16_t cardGap = theme.spaceMd;
  const auto cardW = static_cast<int16_t>((body.width - cardGap * (weather::DAYS - 1)) / weather::DAYS);
  constexpr int16_t DAY_ICON = 48;
  const auto cardH = static_cast<int16_t>(theme.spaceMd * 4 + bodyH * 2 + DAY_ICON);
  for (int i = 0; i < forecast.dayCount; ++i) {
    const weather::Day& day = forecast.days[i];
    const fui::Rect card{static_cast<int16_t>(body.x + i * (cardW + cardGap)), y, cardW, cardH};
    screen.target().stroke(card, ink, 2, theme.controlRadius);
    auto cy = static_cast<int16_t>(card.y + theme.spaceMd);
    auto name = theme.bodyText;
    name.bold = true;
    name.align = fui::TextAlign::Center;
    screen.target().text(fui::Rect{card.x, cy, card.width, bodyH}, dayName(day.date, i == 0), name);
    cy = static_cast<int16_t>(cy + bodyH + theme.spaceMd);
    screen.target().bitmap(
        fui::Rect{static_cast<int16_t>(card.x + (card.width - DAY_ICON) / 2), cy, DAY_ICON, DAY_ICON},
        fui::bitmapFromIcon(iconFor(day.code, false)), fui::BitmapMode::Center, ink);
    cy = static_cast<int16_t>(cy + DAY_ICON + theme.spaceMd);
    char range[24];
    snprintf(range, sizeof(range), "%d° / %d°", weather::toDisplay(day.highC, f), weather::toDisplay(day.lowC, f));
    screen.target().text(fui::Rect{card.x, cy, card.width, bodyH}, range, body2);
  }
}

void WeatherActivity::drawButtons(UiScreen& screen, const fui::Rect body) {
  const auto& theme = screen.theme();
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const int16_t gap = theme.spaceMd;
  const auto bw = static_cast<int16_t>((body.width - gap * (BUTTON_COUNT - 1)) / BUTTON_COUNT);
  const auto by = static_cast<int16_t>(body.bottom() - BUTTON_HEIGHT - theme.spaceLg);
  for (int i = 0; i < BUTTON_COUNT; ++i) {
    const fui::Rect r{static_cast<int16_t>(body.x + i * (bw + gap)), by, bw, BUTTON_HEIGHT};
    const bool isSelected = i == selected && !BoardConfig::hasTouch();
    if (isSelected) {
      screen.target().fill(r, ink, theme.controlRadius);
    } else {
      screen.target().stroke(r, ink, 2, theme.controlRadius);
    }
    auto text = theme.bodyText;
    text.align = fui::TextAlign::Center;
    text.inverted = isSelected;
    const int16_t lh = screen.target().lineHeight(text.font);
    screen.target().text(fui::Rect{r.x, static_cast<int16_t>(r.y + (r.height - lh) / 2), r.width, lh},
                         i == UPDATE ? tr(STR_UPDATE) : tr(STR_WEATHER_UNIT), text);
    if (status != Status::Updating) screen.frame().hit(r, PRESS, static_cast<int16_t>(i), fui::InputTouch);
  }
}

void WeatherActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_APP_WEATHER));
  renderUi();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

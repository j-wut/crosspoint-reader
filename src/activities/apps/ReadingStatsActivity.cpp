#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <TrustedTime.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr int CHART_DAYS = 7;

void formatDuration(char* buf, const size_t size, const uint32_t seconds) {
  const uint32_t minutes = seconds / 60;
  if (minutes < 60) {
    snprintf(buf, size, tr(STR_STATS_MINUTES), static_cast<unsigned>(minutes));
  } else {
    snprintf(buf, size, tr(STR_STATS_HOURS_MINUTES), static_cast<unsigned>(minutes / 60),
             static_cast<unsigned>(minutes % 60));
  }
}

// YYYYMMDD `back` days before `date`.
int32_t daysBefore(const int32_t date, const int back) {
  struct tm t {};
  t.tm_year = date / 10000 - 1900;
  t.tm_mon = date / 100 % 100 - 1;
  t.tm_mday = date % 100 - back;
  t.tm_hour = 12;  // away from DST edges
  mktime(&t);
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}
}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  readingstats::load(stats);
  const int64_t now = trustedtime::trustedNow();
  today = now > 0 ? readingstats::localDate(now) : 0;
  resetUi();
  app.setScreen(&ReadingStatsActivity::screenFn, this);
  requestUpdate();
}

void ReadingStatsActivity::screenFn(UiScreen& screen, void* user) {
  static_cast<ReadingStatsActivity*>(user)->draw(screen);
}

void ReadingStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void ReadingStatsActivity::draw(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width) + metrics.contentSidePadding),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
      static_cast<int16_t>(safe.x + metrics.contentSidePadding)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const int16_t smallH = screen.target().lineHeight(theme.smallText.font);
  const int16_t titleH = screen.target().lineHeight(theme.titleText.font);

  // Today, this month, and the average over days read.
  uint32_t todaySecs = 0;
  uint32_t monthSecs = 0;
  for (const auto& day : stats.days) {
    if (day.date == today) todaySecs = day.seconds;
    if (today != 0 && day.date / 100 == today / 100) monthSecs += day.seconds;
  }
  const auto daysRead = static_cast<uint32_t>(stats.days.size());
  const uint32_t perDay = daysRead > 0 ? stats.totalSeconds / daysRead : 0;

  char values[5][32];
  formatDuration(values[0], sizeof(values[0]), stats.totalSeconds);
  snprintf(values[1], sizeof(values[1]), "%u", static_cast<unsigned>(daysRead));
  formatDuration(values[2], sizeof(values[2]), todaySecs);
  formatDuration(values[3], sizeof(values[3]), monthSecs);
  formatDuration(values[4], sizeof(values[4]), perDay);
  const char* labels[5] = {tr(STR_STATS_TOTAL), tr(STR_STATS_DAYS_READ), tr(STR_STATS_TODAY), tr(STR_STATS_MONTH),
                           tr(STR_STATS_PER_DAY)};

  // Two wide cards, then three narrow ones.
  const auto drawCard = [&](const fui::Rect r, const int i) {
    screen.target().stroke(r, ink, 2, theme.controlRadius);
    auto label = theme.smallText;
    label.align = fui::TextAlign::Center;
    auto value = theme.titleText;
    value.bold = true;
    value.align = fui::TextAlign::Center;
    const auto top = static_cast<int16_t>(r.y + (r.height - smallH - titleH - theme.spaceSm) / 2);
    screen.target().text(fui::Rect{r.x, top, r.width, titleH}, values[i], value);
    screen.target().text(fui::Rect{static_cast<int16_t>(r.x + 4), static_cast<int16_t>(top + titleH + theme.spaceSm),
                                   static_cast<int16_t>(r.width - 8), smallH},
                         labels[i], label);
  };
  const int16_t gap = theme.spaceMd;
  const auto cardH = static_cast<int16_t>(titleH + smallH + theme.spaceLg * 2);
  fui::Rect row = screen.takeTop(cardH, gap);
  const auto half = static_cast<int16_t>((row.width - gap) / 2);
  drawCard(fui::Rect{row.x, row.y, half, cardH}, 0);
  drawCard(fui::Rect{static_cast<int16_t>(row.x + half + gap), row.y, half, cardH}, 1);
  row = screen.takeTop(cardH, static_cast<int16_t>(theme.spaceLg * 2));
  const auto third = static_cast<int16_t>((row.width - 2 * gap) / 3);
  for (int i = 0; i < 3; ++i) {
    drawCard(fui::Rect{static_cast<int16_t>(row.x + i * (third + gap)), row.y, third, cardH}, 2 + i);
  }

  auto heading = theme.bodyText;
  heading.bold = true;
  screen.target().text(screen.takeTop(screen.target().lineHeight(heading.font), theme.spaceMd), tr(STR_STATS_LAST_7),
                       heading);
  if (today == 0) {
    auto hint = theme.smallText;
    hint.maxLines = 3;
    screen.target().text(screen.takeTop(static_cast<int16_t>(smallH * 3), 0), tr(STR_STATS_NO_CLOCK), hint);
    return;
  }

  // Seven-day bars, oldest on the left, scaled to the busiest day (at least 30 min).
  const fui::Rect chart = screen.takeTop(static_cast<int16_t>(std::min<int>(220, screen.body().height)), 0);
  uint32_t secs[CHART_DAYS] = {};
  int32_t dates[CHART_DAYS] = {};
  uint32_t peak = 30 * 60;
  for (int i = 0; i < CHART_DAYS; ++i) {
    dates[i] = daysBefore(today, CHART_DAYS - 1 - i);
    for (const auto& day : stats.days) {
      if (day.date == dates[i]) secs[i] = day.seconds;
    }
    peak = std::max(peak, secs[i]);
  }
  const auto slot = static_cast<int16_t>(chart.width / CHART_DAYS);
  const auto barW = static_cast<int16_t>(slot * 5 / 10);
  const auto barMax = static_cast<int16_t>(chart.height - 2 * smallH - theme.spaceSm * 2);
  const auto baseY = static_cast<int16_t>(chart.y + smallH + theme.spaceSm + barMax);
  screen.target().fill(fui::Rect{chart.x, baseY, chart.width, 2}, ink);
  auto label = theme.smallText;
  label.align = fui::TextAlign::Center;
  for (int i = 0; i < CHART_DAYS; ++i) {
    const auto x = static_cast<int16_t>(chart.x + i * slot);
    const auto h = static_cast<int16_t>(static_cast<uint64_t>(barMax) * secs[i] / peak);
    if (h > 0) {
      screen.target().fill(fui::Rect{static_cast<int16_t>(x + (slot - barW) / 2), static_cast<int16_t>(baseY - h),
                                     barW, h},
                           ink, 3);
    }
    char top[12];
    if (secs[i] > 0) {
      snprintf(top, sizeof(top), "%u", static_cast<unsigned>((secs[i] + 59) / 60));
      screen.target().text(fui::Rect{x, static_cast<int16_t>(baseY - h - smallH - 2), slot, smallH}, top, label);
    }
    char day[4];
    snprintf(day, sizeof(day), "%d", static_cast<int>(dates[i] % 100));
    screen.target().text(fui::Rect{x, static_cast<int16_t>(baseY + theme.spaceSm), slot, smallH}, day, label);
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_APP_READING_STATS));
  renderUi();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#pragma once

#include <FreeInkUICore.h>

#include <cstring>

// Seven-segment digits for timer readouts, far larger than the built-in UI
// fonts. Draws "0"-"9" and ":"; other characters leave a gap.
namespace bigdigits {

// Width of `text` at digit height `height`.
inline int16_t width(const char* text, const int16_t height) {
  const int16_t digitW = static_cast<int16_t>(height / 2);
  const int16_t gap = static_cast<int16_t>(height / 8);
  int16_t w = 0;
  for (const char* c = text; *c; ++c) {
    w = static_cast<int16_t>(w + (*c == ':' ? height / 5 : digitW) + gap);
  }
  return w > 0 ? static_cast<int16_t>(w - gap) : 0;
}

inline void draw(freeink::ui::DrawTarget& target, int16_t x, const int16_t y, const char* text, const int16_t height) {
  namespace fui = freeink::ui;
  // Segment bits: a b c d e f g (top, top-right, bottom-right, bottom,
  // bottom-left, top-left, middle).
  static constexpr uint8_t SEGMENTS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const int16_t w = static_cast<int16_t>(height / 2);
  const int16_t t = static_cast<int16_t>(height / 9);  // stroke thickness
  const int16_t gap = static_cast<int16_t>(height / 8);
  const int16_t half = static_cast<int16_t>(height / 2);
  for (const char* c = text; *c; ++c) {
    if (*c == ':') {
      const int16_t dot = t;
      const int16_t cx = static_cast<int16_t>(x + (height / 5 - dot) / 2);
      target.fill(fui::Rect{cx, static_cast<int16_t>(y + height / 3 - dot / 2), dot, dot}, ink, dot / 2);
      target.fill(fui::Rect{cx, static_cast<int16_t>(y + height * 2 / 3 - dot / 2), dot, dot}, ink, dot / 2);
      x = static_cast<int16_t>(x + height / 5 + gap);
      continue;
    }
    if (*c >= '0' && *c <= '9') {
      const uint8_t s = SEGMENTS[*c - '0'];
      const auto seg = [&](const int bit, const fui::Rect r) {
        if (s & (1 << bit)) target.fill(r, ink, static_cast<uint8_t>(t / 2));
      };
      seg(0, fui::Rect{x, y, w, t});
      seg(1, fui::Rect{static_cast<int16_t>(x + w - t), y, t, static_cast<int16_t>(half + t / 2)});
      seg(2, fui::Rect{static_cast<int16_t>(x + w - t), static_cast<int16_t>(y + half - t / 2), t,
                       static_cast<int16_t>(height - half + t / 2)});
      seg(3, fui::Rect{x, static_cast<int16_t>(y + height - t), w, t});
      seg(4, fui::Rect{x, static_cast<int16_t>(y + half - t / 2), t, static_cast<int16_t>(height - half + t / 2)});
      seg(5, fui::Rect{x, y, t, static_cast<int16_t>(half + t / 2)});
      seg(6, fui::Rect{x, static_cast<int16_t>(y + half - t / 2), w, t});
    }
    x = static_cast<int16_t>(x + w + gap);
  }
}

}  // namespace bigdigits

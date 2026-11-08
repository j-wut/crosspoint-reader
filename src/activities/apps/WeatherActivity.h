#pragma once

#include "activities/Activity.h"
#include "apps/Weather.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Current weather and a 3-day forecast for the place the network is in.
// Shows the last saved forecast; Update joins Wi-Fi and fetches a new one.
class WeatherActivity final : public Activity, private UiAppHost {
 public:
  WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Weather", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Button : uint8_t { UPDATE, UNIT, BUTTON_COUNT };
  enum class Status : uint8_t { Idle, Updating, NoWifi, NoLocation, NoForecast };

  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void drawForecast(UiScreen& screen, freeink::ui::Rect body);
  void drawButtons(UiScreen& screen, freeink::ui::Rect body);
  void press(Button button);
  void startUpdate();
  void runUpdate();
  void stopWifi();

  weather::Forecast forecast;
  ButtonNavigator buttonNavigator;
  Status status = Status::Idle;
  int selected = UPDATE;
  int pending = -1;
  bool ownsWifi = false;
};

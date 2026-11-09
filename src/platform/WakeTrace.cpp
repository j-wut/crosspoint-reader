#include "WakeTrace.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <time.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "util/DebugLog.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <BatteryMonitor.h>
#include <PowerManager.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <esp_system.h>
#endif

namespace {
constexpr uint32_t SLEEP_TRACE_MAGIC = 0x534C5054;  // 'SLPT'

struct SleepTrace {
  uint32_t magic;
  uint32_t seq;
  uint8_t stage;
  uint32_t uptimeMs;
  int64_t epoch;
  uint16_t batteryMv;
  uint8_t usb;
  uint8_t charging;
};

// Captured at the top of setup(), reported once the SD card is up.
struct BootCapture {
  int resetReason = -1;
  int wakeCause = -1;
  uint64_t ext1Pins = 0;
  bool abortedSleep = false;
  int abortedCause = 0;
  int abortedPinLevel = 0;
  bool hadTrace = false;
  SleepTrace previous{};
  // Previous boot's log ring (RTC memory), at most 16 lines of 256 bytes.
  // Freed after the report is written.
  std::string previousLogs;
};
BootCapture capture;
}  // namespace

RTC_NOINIT_ATTR SleepTrace sleepTrace;

static const char* resetReasonName(const int reason) {
#if defined(ARDUINO_ARCH_ESP32)
  switch (static_cast<esp_reset_reason_t>(reason)) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXT";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_USB:
      return "USB";
    case ESP_RST_JTAG:
      return "JTAG";
    case ESP_RST_EFUSE:
      return "EFUSE";
    case ESP_RST_PWR_GLITCH:
      return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP:
      return "CPU_LOCKUP";
    default:
      break;
  }
#endif
  (void)reason;
  return "UNKNOWN";
}

static void readBattery(uint16_t& millivolts, bool& usb, bool& charging) {
  millivolts = 0;
  usb = false;
  charging = false;
#if defined(ARDUINO_ARCH_ESP32)
  const BatteryMonitor battery;
  millivolts = battery.readMillivolts();
  bool known = false;
  usb = battery.isExternalPowerPresent(&known);
  charging = battery.isCharging();
#endif
}

void wakeTraceCapture() {
#if defined(ARDUINO_ARCH_ESP32)
  capture.resetReason = static_cast<int>(esp_reset_reason());
  capture.wakeCause = static_cast<int>(esp_sleep_get_wakeup_cause());
#if SOC_PM_SUPPORT_EXT1_WAKEUP
  capture.ext1Pins = esp_sleep_get_ext1_wakeup_status();
#endif
  const auto aborted = freeink::PowerManager::takeAbortedSleepInfo();
  capture.abortedSleep = aborted.aborted;
  capture.abortedCause = aborted.wakeupCause;
  capture.abortedPinLevel = aborted.wakePinLevel;
#endif
  capture.hadTrace = sleepTrace.magic == SLEEP_TRACE_MAGIC;
  if (capture.hadTrace) capture.previous = sleepTrace;
  capture.previousLogs = getLastLogs();

  const uint32_t seq = capture.hadTrace ? sleepTrace.seq : 0;
  sleepTrace = SleepTrace{};
  sleepTrace.magic = SLEEP_TRACE_MAGIC;
  sleepTrace.seq = seq;
  sleepTrace.stage = static_cast<uint8_t>(SleepStage::Awake);
}

void wakeTraceStage(const SleepStage stage) {
  if (sleepTrace.magic != SLEEP_TRACE_MAGIC) {
    sleepTrace = SleepTrace{};
    sleepTrace.magic = SLEEP_TRACE_MAGIC;
  }
  if (stage == SleepStage::Started) {
    sleepTrace.seq++;
    uint16_t mv;
    bool usb;
    bool charging;
    readBattery(mv, usb, charging);
    sleepTrace.batteryMv = mv;
    sleepTrace.usb = usb;
    sleepTrace.charging = charging;
  }
  sleepTrace.stage = static_cast<uint8_t>(stage);
  sleepTrace.uptimeMs = millis();
  sleepTrace.epoch = static_cast<int64_t>(time(nullptr));
}

void appendWakeLog(const char* fmt, ...) {
  char line[320];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  LOG_INF("WAKE", "%s", line);
  debuglog::line(debuglog::Feature::WakeUp, "%s", line);
}

void wakeTraceWriteBootReport(const char* wakeReason, const bool stateLoaded, const bool showBootScreen,
                              const char* resume, const bool panic) {
  uint16_t mv;
  bool usb;
  bool charging;
  readBattery(mv, usb, charging);
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minHeap = ESP.getMinFreeHeap();
  uint32_t freePsram = 0;
#if defined(ARDUINO_ARCH_ESP32)
  freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif

  appendWakeLog(
      "boot: reset=%s(%d) cause=%d ext1=0x%llx reason=%s abortedSleep=%d(cause=%d pin=%d) stateLoaded=%d "
      "showBootScreen=%d resume=%s panic=%d battMv=%u usb=%d charging=%d heap=%lu minHeap=%lu psram=%lu",
      resetReasonName(capture.resetReason), capture.resetReason, capture.wakeCause,
      static_cast<unsigned long long>(capture.ext1Pins), wakeReason, capture.abortedSleep ? 1 : 0, capture.abortedCause,
      capture.abortedPinLevel, stateLoaded ? 1 : 0, showBootScreen ? 1 : 0, resume, panic ? 1 : 0, mv, usb ? 1 : 0,
      charging ? 1 : 0, static_cast<unsigned long>(freeHeap), static_cast<unsigned long>(minHeap),
      static_cast<unsigned long>(freePsram));

  if (capture.hadTrace) {
    const SleepTrace& p = capture.previous;
    const long long now = static_cast<long long>(time(nullptr));
    appendWakeLog("boot: previous run #%lu stage=%u uptimeMs=%lu at=%lld (%llds ago) battMv=%u usb=%u charging=%u",
                  static_cast<unsigned long>(p.seq), p.stage, static_cast<unsigned long>(p.uptimeMs),
                  static_cast<long long>(p.epoch), now - static_cast<long long>(p.epoch), p.batteryMv, p.usb,
                  p.charging);
  } else {
    appendWakeLog("boot: no previous run record (power was lost, or first boot of this build)");
  }

  // The previous boot's last log lines, when this boot was not a plain wake
  // from sleep or a power-on.
#if defined(ARDUINO_ARCH_ESP32)
  const bool plainStart = capture.resetReason == ESP_RST_DEEPSLEEP || capture.resetReason == ESP_RST_POWERON;
#else
  const bool plainStart = true;
#endif
  if ((strcmp(resume, "splash") == 0 || !plainStart) && !capture.previousLogs.empty()) {
    appendWakeLog("boot: previous run's last log lines:");
    size_t start = 0;
    while (start < capture.previousLogs.size()) {
      size_t end = capture.previousLogs.find('\n', start);
      if (end == std::string::npos) end = capture.previousLogs.size();
      if (end > start) {
        appendWakeLog("  | %.*s", static_cast<int>(std::min<size_t>(end - start, 280)),
                      capture.previousLogs.c_str() + start);
      }
      start = end + 1;
    }
  }
  std::string().swap(capture.previousLogs);
}

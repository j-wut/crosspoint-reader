#include "WakeTrace.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#endif

void addToLogRingBuffer(const char* message);  // Logging.cpp

namespace {
constexpr uint32_t SLEEP_TRACE_MAGIC = 0x534C5032;  // 'SLP2'
constexpr size_t STAGE_COUNT = static_cast<size_t>(SleepStage::Count);
constexpr size_t BOOT_STAGE_COUNT = static_cast<size_t>(BootStage::Count);

constexpr const char* STAGE_NAMES[] = {
    "Awake",           "Started",       "StateSaved", "ScreenDrawn", "UsbHold",          "UsbHoldDone",
    "DisplayOff",      "StorageParked", "ShipMode",   "DeepSleep",   "BluetoothStopped", "PluginEventsSent",
    "SleepFrameSaved", "WifiOff",       "TiltOff",    "InputParked", "SerialClosed",     "PinsHeld",
    "RailsDown",       "WaitRelease",   "WakeArmed",  "EnterSleep",  "RejectedWake",
};
static_assert(sizeof(STAGE_NAMES) / sizeof(STAGE_NAMES[0]) == STAGE_COUNT, "one name per sleep stage");

constexpr const char* BOOT_STAGE_NAMES[] = {
    "None",           "Captured",    "SystemReady",  "GpioReady",    "WakeChecked", "StorageReady",
    "SettingsLoaded", "WakeDecided", "BootReported", "DisplayReady", "SetupDone",   "FirstLoopDone",
};
static_assert(sizeof(BOOT_STAGE_NAMES) / sizeof(BOOT_STAGE_NAMES[0]) == BOOT_STAGE_COUNT, "one name per boot stage");

struct SleepTrace {
  uint32_t magic;
  uint32_t seq;
  uint8_t stage;
  uint32_t uptimeMs;
  int64_t epoch;
  uint16_t batteryMv;
  uint8_t usb;
  uint8_t charging;
  // millis() when each stage was reached in this run, 0 = not reached.
  uint32_t stageMs[STAGE_COUNT];
  uint8_t bootStage;
  uint32_t bootStageMs[BOOT_STAGE_COUNT];
  uint32_t bootCount;
  uint16_t lowestBatteryMv;  // lowest reading during sleep entry
  uint32_t minFreeHeap;      // ESP.getMinFreeHeap() at the last stage
  char trigger[24];
  char activity[24];
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

static const char* stageName(const uint8_t stage) { return stage < STAGE_COUNT ? STAGE_NAMES[stage] : "?"; }
static const char* bootStageName(const uint8_t stage) {
  return stage < BOOT_STAGE_COUNT ? BOOT_STAGE_NAMES[stage] : "?";
}

static void copyLabel(char* out, const size_t size, const char* text) { snprintf(out, size, "%s", text ? text : ""); }

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

namespace {
// While Debug Logging is on, keeps every log line from sleep entry (any task,
// plus ESP-IDF output) until the main task can write it to the card.
namespace sleeplog {
// Allocated on the way to sleep and never freed: the chip resets on wake, and
// other tasks may still be inside sink() until then.
constexpr size_t CAPACITY = 8 * 1024;
char* buffer = nullptr;
size_t used = 0;
size_t dropped = 0;
bool open = false;  // buffer is collecting; card writes allowed
bool started = false;
bool headerWritten = false;
TaskHandle_t owner = nullptr;
#if defined(ARDUINO_ARCH_ESP32)
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t previousVprintf = nullptr;
esp_log_level_t previousLevel = ESP_LOG_ERROR;
#define SLEEPLOG_LOCK() portENTER_CRITICAL_SAFE(&lock)
#define SLEEPLOG_UNLOCK() portEXIT_CRITICAL_SAFE(&lock)
#else
#define SLEEPLOG_LOCK()
#define SLEEPLOG_UNLOCK()
#endif

void append(const char* text, const size_t len) {
  if (!open || !buffer) return;
  SLEEPLOG_LOCK();
  if (used + len <= CAPACITY) {
    memcpy(buffer + used, text, len);
    used += len;
  } else {
    dropped += len;
  }
  SLEEPLOG_UNLOCK();
}

void sink(const char* line) { append(line, strlen(line)); }

#if defined(ARDUINO_ARCH_ESP32)
// ESP-IDF errors and warnings also go to the RTC log ring, which survives a
// crash, and keep doing so after the card is parked.
int idfCapture(const char* format, va_list args) {
  va_list copy;
  va_copy(copy, args);
  char text[224];
  const int len = vsnprintf(text, sizeof(text), format, copy);
  va_end(copy);
  if (len > 0) {
    const char* body = text;
    if (body[0] == '\033') {
      const char* end = strchr(body, 'm');
      body = end ? end + 1 : body;
    }
    char entry[240];
    const int n = snprintf(entry, sizeof(entry), "[%lu] [IDF] %s", millis(), body);
    if (n > 0) {
      append(entry, std::min(static_cast<size_t>(n), sizeof(entry) - 1));
      if (body[0] == 'E' || body[0] == 'W') addToLogRingBuffer(entry);
    }
  }
  return previousVprintf ? previousVprintf(format, args) : len;
}
#endif

// Writes the kept lines to the wake-up log. Main task only: SdFat must not be
// used from two tasks at once.
void flush() {
  if (!open || !buffer || xTaskGetCurrentTaskHandle() != owner) return;
  // A transfer log capture ending during sleep entry unhooks the sink.
  setLogSink(sink);
  SLEEPLOG_LOCK();
  const size_t total = used;
  const size_t lost = dropped;
  SLEEPLOG_UNLOCK();
  if (total == 0 && lost == 0) return;
  {
    debuglog::File file = debuglog::open(debuglog::Feature::WakeUp);
    if (file && !headerWritten) {
      headerWritten = true;
      const time_t now = time(nullptr);
      struct tm local;
      localtime_r(&now, &local);
      file.printf("[%04d-%02d-%02d %02d:%02d:%02d +%lums] sleep log:\n", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, static_cast<unsigned long>(millis()));
    }
    if (file) {
      file.write(reinterpret_cast<const uint8_t*>(buffer), total);
      if (lost > 0) file.printf("  (sleep log full, %u bytes dropped)\n", static_cast<unsigned>(lost));
      file.close();
    }
  }
  SLEEPLOG_LOCK();
  const size_t extra = used - total;
  memmove(buffer, buffer + total, extra);
  used = extra;
  dropped = 0;
  SLEEPLOG_UNLOCK();
}
}  // namespace sleeplog
}  // namespace

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
  const uint32_t bootCount = capture.hadTrace ? sleepTrace.bootCount : 0;
  sleepTrace = SleepTrace{};
  sleepTrace.magic = SLEEP_TRACE_MAGIC;
  sleepTrace.seq = seq;
  sleepTrace.bootCount = bootCount + 1;
  sleepTrace.stage = static_cast<uint8_t>(SleepStage::Awake);
  wakeTraceBoot(BootStage::Captured);
}

void wakeTraceBoot(const BootStage stage) {
  const auto index = static_cast<size_t>(stage);
  if (sleepTrace.magic != SLEEP_TRACE_MAGIC || index >= BOOT_STAGE_COUNT) return;
  sleepTrace.bootStage = static_cast<uint8_t>(stage);
  sleepTrace.bootStageMs[index] = millis();
}

void wakeTraceStage(const SleepStage stage) {
  if (sleepTrace.magic != SLEEP_TRACE_MAGIC) {
    sleepTrace = SleepTrace{};
    sleepTrace.magic = SLEEP_TRACE_MAGIC;
  }
  const auto index = static_cast<size_t>(stage);
  const uint32_t now = millis();
  if (stage == SleepStage::Started || stage == SleepStage::RejectedWake) {
    sleepTrace.seq++;
    memset(sleepTrace.stageMs, 0, sizeof(sleepTrace.stageMs));
    uint16_t mv;
    bool usb;
    bool charging;
    readBattery(mv, usb, charging);
    sleepTrace.batteryMv = mv;
    sleepTrace.usb = usb;
    sleepTrace.charging = charging;
    sleepTrace.lowestBatteryMv = mv;
  }
  sleepTrace.stage = static_cast<uint8_t>(stage);
  if (index < STAGE_COUNT) sleepTrace.stageMs[index] = now;
  sleepTrace.uptimeMs = now;
  sleepTrace.epoch = static_cast<int64_t>(time(nullptr));
  sleepTrace.minFreeHeap = ESP.getMinFreeHeap();

  if (!sleeplog::started) return;
  if (sleeplog::open) {
    // Battery sag under the panel refresh can brown the chip out.
    uint16_t mv;
    bool usb;
    bool charging;
    readBattery(mv, usb, charging);
    if (mv > 0 && (sleepTrace.lowestBatteryMv == 0 || mv < sleepTrace.lowestBatteryMv)) {
      sleepTrace.lowestBatteryMv = mv;
    }
    LOG_INF("SLPT", "step %s at %lums: batt=%umV usb=%d charging=%d heap=%lu minHeap=%lu maxBlock=%lu stack=%u",
            stageName(sleepTrace.stage), static_cast<unsigned long>(now), mv, usb ? 1 : 0, charging ? 1 : 0,
            static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMinFreeHeap()),
            static_cast<unsigned long>(ESP.getMaxAllocHeap()),
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    sleeplog::flush();
  } else {
    // Card parked and rails may be off: RTC ring and serial only.
    LOG_INF("SLPT", "step %s at %lums: heap=%lu", stageName(sleepTrace.stage), static_cast<unsigned long>(now),
            static_cast<unsigned long>(ESP.getFreeHeap()));
  }
}

// Steps reported by name from HalPowerManager::startDeepSleep().
void halSleepStep(const char* step) {
  for (size_t i = 0; i < STAGE_COUNT; i++) {
    if (strcmp(step, STAGE_NAMES[i]) == 0) {
      wakeTraceStage(static_cast<SleepStage>(i));
      return;
    }
  }
  LOG_INF("SLPT", "unknown sleep step %s", step);
}

void wakeTraceSleepLogBegin(const char* trigger, const char* activity, const bool fromTimeout, const bool powerOff) {
  copyLabel(sleepTrace.trigger, sizeof(sleepTrace.trigger), trigger);
  copyLabel(sleepTrace.activity, sizeof(sleepTrace.activity), activity);
  if (!debuglog::enabled()) return;
  sleeplog::owner = xTaskGetCurrentTaskHandle();
  if (!sleeplog::buffer) {
#if defined(ARDUINO_ARCH_ESP32)
    sleeplog::buffer = static_cast<char*>(heap_caps_malloc(sleeplog::CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
    if (!sleeplog::buffer) sleeplog::buffer = static_cast<char*>(malloc(sleeplog::CAPACITY));
  }
  sleeplog::started = true;
  if (!sleeplog::buffer) {
    appendWakeLog("sleep: no memory for the sleep log (%u bytes); logging steps only",
                  static_cast<unsigned>(sleeplog::CAPACITY));
  }
  sleeplog::used = 0;
  sleeplog::dropped = 0;
  sleeplog::open = sleeplog::buffer != nullptr;
  setLogSink(sleeplog::sink);
#if defined(ARDUINO_ARCH_ESP32)
  sleeplog::previousLevel = esp_log_level_get("*");
  esp_log_level_set("*", ESP_LOG_INFO);
  sleeplog::previousVprintf = esp_log_set_vprintf(sleeplog::idfCapture);
#endif
  appendWakeLog(
      "sleep: begin #%lu trigger=%s activity=%s timeout=%d powerOff=%d uptimeMs=%lu heap=%lu minHeap=%lu "
      "maxBlock=%lu psram=%lu stack=%u tasks=%u",
      static_cast<unsigned long>(sleepTrace.seq + 1), sleepTrace.trigger, sleepTrace.activity, fromTimeout ? 1 : 0,
      powerOff ? 1 : 0, millis(), static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getMinFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()),
#if defined(ARDUINO_ARCH_ESP32)
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), static_cast<unsigned>(uxTaskGetNumberOfTasks()));
#else
      0UL, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), 0U);
#endif
}

void wakeTraceSleepLogEnd() {
  if (!sleeplog::started) return;
  appendWakeLog("sleep: parking the card; later steps are in the RTC record (next boot reports them)");
  sleeplog::flush();
  sleeplog::open = false;
  setLogSink(nullptr);
#if defined(ARDUINO_ARCH_ESP32)
  // The ESP-IDF hook stays installed so driver errors from the last steps
  // still reach the RTC log ring.
  esp_log_level_set("*", sleeplog::previousLevel);
#endif
}

void appendWakeLog(const char* fmt, ...) {
  char line[320];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  LOG_INF("WAKE", "%s", line);
  // While the sleep log is collecting, the LOG line above already reaches the file.
  if (!sleeplog::open) debuglog::line(debuglog::Feature::WakeUp, "%s", line);
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

  appendWakeLog("boot: run #%lu", static_cast<unsigned long>(sleepTrace.bootCount));
  if (capture.hadTrace) {
    const SleepTrace& p = capture.previous;
    const long long now = static_cast<long long>(time(nullptr));
    appendWakeLog(
        "boot: previous run #%lu stage=%s(%u) uptimeMs=%lu at=%lld (%llds ago) battMv=%u lowestMv=%u usb=%u "
        "charging=%u minHeap=%lu trigger=%s activity=%s",
        static_cast<unsigned long>(p.seq), stageName(p.stage), p.stage, static_cast<unsigned long>(p.uptimeMs),
        static_cast<long long>(p.epoch), now - static_cast<long long>(p.epoch), p.batteryMv, p.lowestBatteryMv, p.usb,
        p.charging, static_cast<unsigned long>(p.minFreeHeap), p.trigger, p.activity);

    char steps[320];
    size_t len = 0;
    steps[0] = '\0';
    for (size_t i = 1; i < STAGE_COUNT && len < sizeof(steps); i++) {
      if (p.stageMs[i] == 0) continue;
      len += snprintf(steps + len, sizeof(steps) - len, " %s@%lu", STAGE_NAMES[i],
                      static_cast<unsigned long>(p.stageMs[i]));
    }
    appendWakeLog("boot: previous sleep steps (ms):%s", len > 0 ? steps : " none");

    len = 0;
    steps[0] = '\0';
    for (size_t i = 1; i < BOOT_STAGE_COUNT && len < sizeof(steps); i++) {
      if (p.bootStageMs[i] == 0) continue;
      len += snprintf(steps + len, sizeof(steps) - len, " %s@%lu", BOOT_STAGE_NAMES[i],
                      static_cast<unsigned long>(p.bootStageMs[i]));
    }
    appendWakeLog("boot: previous run's start-up got to %s, steps (ms):%s", bootStageName(p.bootStage),
                  len > 0 ? steps : " none");
  } else {
    appendWakeLog("boot: no previous run record (power was lost, or first boot of this build)");
  }

#if defined(ARDUINO_ARCH_ESP32)
  const int reset = capture.resetReason;
  const char* verdict = "normal";
  if (reset == ESP_RST_DEEPSLEEP) {
    verdict = "woke from deep sleep";
  } else if (reset == ESP_RST_POWERON) {
    verdict =
        capture.hadTrace ? "power-on reset (RTC kept: power dipped, or USB/flash reset)" : "power-on (power was lost)";
  } else if (capture.abortedSleep) {
    verdict = "deep sleep entry was rejected by the chip, firmware restarted";
  } else if (capture.hadTrace && capture.previous.stage != static_cast<uint8_t>(SleepStage::Awake)) {
    verdict = "CRASH/RESET DURING SLEEP ENTRY (see previous stage)";
  } else if (capture.hadTrace && capture.previous.bootStage < static_cast<uint8_t>(BootStage::SetupDone)) {
    verdict = "CRASH/RESET DURING START-UP (see previous start-up stage)";
  } else if (reset == ESP_RST_SW) {
    verdict = "software restart";
  } else {
    verdict = "CRASH/RESET WHILE AWAKE";
  }
  appendWakeLog("boot: verdict: %s", verdict);
#endif

  if (panic) {
    appendWakeLog("boot: crash report (also in /crash_report.txt):");
    const std::string info = HalSystem::getPanicInfo(true);
    size_t start = 0;
    while (start < info.size()) {
      size_t end = info.find('\n', start);
      if (end == std::string::npos) end = info.size();
      if (end > start) {
        appendWakeLog("  ! %.*s", static_cast<int>(std::min<size_t>(end - start, 280)), info.c_str() + start);
      }
      start = end + 1;
    }
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

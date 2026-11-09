#include "BooxDiag.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalGPIO.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "BooxSd.h"

void addToLogRingBuffer(const char* message);  // Logging.cpp

namespace booxdiag {
namespace {

constexpr size_t CAPACITY = 64 * 1024;
// SD card driver tags, logged in full.
constexpr const char* DEBUG_TAGS[] = {"sdmmc_req", "sdmmc_cmd",    "sdmmc_common", "sdmmc_sd",
                                      "sdmmc_periph", "sdmmc_io", "sdmmc_init",   "vfs_fat_sdmmc",
                                      "vfs_fat",   "httpd",        "httpd_parse",
                                      "httpd_uri", "httpd_sess"};
// Allocated once and never freed: another task may still be inside sink() or
// capture() just after the destructor unhooks them.
char* buffer = nullptr;
size_t used = 0;
size_t dropped = 0;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t previous = nullptr;
esp_log_level_t previousLevel = ESP_LOG_ERROR;

void append(const char* text, const size_t len) {
  if (!buffer) return;
  portENTER_CRITICAL_SAFE(&lock);
  if (used + len <= CAPACITY) {
    memcpy(buffer + used, text, len);
    used += len;
  } else {
    dropped += len;
  }
  portEXIT_CRITICAL_SAFE(&lock);
}

void sink(const char* line) { append(line, strlen(line)); }

int capture(const char* format, va_list args) {
  va_list copy;
  va_copy(copy, args);
  char line[224];
  const int len = vsnprintf(line, sizeof(line), format, copy);
  va_end(copy);
  if (len > 0) {
    const char* text = line;
    if (text[0] == '\033') {
      const char* end = strchr(text, 'm');
      text = end ? end + 1 : text;
    }
    char entry[240];
    const int n = snprintf(entry, sizeof(entry), "[%lu] [IDF] %s", millis(), text);
    if (n > 0) append(entry, std::min(static_cast<size_t>(n), sizeof(entry) - 1));
    if (text[0] == 'E' || text[0] == 'W') addToLogRingBuffer(entry);
  }
  return previous ? previous(format, args) : len;
}

}  // namespace

Capture::Capture() {
  if (!buffer) buffer = static_cast<char*>(heap_caps_malloc(CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  used = 0;
  dropped = 0;
  previousLevel = esp_log_level_get("*");
  esp_log_level_set("*", ESP_LOG_INFO);
  for (const char* tag : DEBUG_TAGS) esp_log_level_set(tag, ESP_LOG_DEBUG);
  previous = esp_log_set_vprintf(capture);
  setLogSink(sink);
  g_logDisplayRefresh = true;
}

Capture::~Capture() {
  g_logDisplayRefresh = false;
  setLogSink(nullptr);
  esp_log_set_vprintf(previous);
  esp_log_level_set("*", previousLevel);
}

void Capture::dumpAndReset(Print& out) {
  if (!buffer) {
    out.println("  (no PSRAM for the transfer log)");
    return;
  }
  // Copy out under the lock in pieces, so new lines can keep arriving.
  portENTER_CRITICAL_SAFE(&lock);
  const size_t total = used;
  const size_t lost = dropped;
  portEXIT_CRITICAL_SAFE(&lock);
  out.printf("  transfer log (%u bytes, %u bytes dropped):\n", static_cast<unsigned>(total),
             static_cast<unsigned>(lost));
  out.write(reinterpret_cast<const uint8_t*>(buffer), total);
  out.println();
  portENTER_CRITICAL_SAFE(&lock);
  // Keep lines that arrived while writing.
  const size_t extra = used - total;
  memmove(buffer, buffer + total, extra);
  used = extra;
  dropped = 0;
  portEXIT_CRITICAL_SAFE(&lock);
}

size_t LogFile::write(const uint8_t c) { return write(&c, 1); }

size_t LogFile::write(const uint8_t* buf, const size_t len) {
  if (useVfs_) return vfs_ ? vfs_.write(buf, len) : 0;
  return hal_ ? hal_.write(buf, len) : 0;
}

void LogFile::flush() {
  if (useVfs_) {
    if (vfs_) vfs_.flush();
  } else if (hal_) {
    hal_.flush();
  }
}

void LogFile::close() {
  if (useVfs_) {
    if (vfs_) vfs_.close();
  } else if (hal_) {
    hal_.close();
  }
}

size_t LogFile::size() {
  if (useVfs_) return vfs_ ? vfs_.size() : 0;
  return hal_ ? hal_.size() : 0;
}

LogFile::operator bool() const { return useVfs_ ? static_cast<bool>(vfs_) : static_cast<bool>(hal_); }

LogFile openLog() {
  LogFile log;
  if (booxsd::stockMounted()) {
    fs::FS& fs = booxsd::fs();
    bool fresh = true;
    if (fs.exists(LOG_PATH)) {
      fs::File existing = fs.open(LOG_PATH, FILE_READ);
      fresh = !existing || existing.size() > LOG_MAX_BYTES;
    }
    log.useVfs_ = true;
    log.vfs_ = fs.open(LOG_PATH, fresh ? FILE_WRITE : FILE_APPEND);
    return log;
  }
  bool fresh = true;
  if (Storage.exists(LOG_PATH)) {
    HalFile existing = Storage.open(LOG_PATH);
    fresh = !existing || existing.size() > LOG_MAX_BYTES;
  }
  log.hal_ = Storage.open(LOG_PATH, fresh ? (O_WRONLY | O_CREAT | O_TRUNC) : (O_WRONLY | O_CREAT | O_APPEND));
  return log;
}

void logSystemState(const char* when) {
  LOG_INF("BOOX", "%s: heap %u max %u, DMA %u max %u, PSRAM %u, heap ok %d, RSSI %d, wifi %d, battery %u%%, stack %u",
          when, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
          static_cast<unsigned>(ESP.getFreePsram()), heap_caps_check_integrity_all(true) ? 1 : 0,
          static_cast<int>(WiFi.RSSI()), static_cast<int>(WiFi.status()),
          static_cast<unsigned>(powerManager.getBatteryPercentage()),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void logPowerState(const char* when) {
  wifi_ps_type_t ps = WIFI_PS_NONE;
  const bool psOk = esp_wifi_get_ps(&ps) == ESP_OK;
  int8_t txPower = 0;
  const bool txOk = esp_wifi_get_max_tx_power(&txPower) == ESP_OK;
  const int8_t rail = BoardConfig::ACTIVE.sd.powerEnable;
  LOG_INF("BOOX", "%s: wifi ps %d, max tx %d (0.25 dBm), cpu %u MHz, sd/panel rail gpio %d = %d, light %d at %u%%, usb %d",
          when, psOk ? static_cast<int>(ps) : -1, txOk ? static_cast<int>(txPower) : -1,
          static_cast<unsigned>(getCpuFrequencyMhz()), rail, rail >= 0 ? gpio_get_level(static_cast<gpio_num_t>(rail)) : -1,
          Frontlight.isOn() ? 1 : 0, static_cast<unsigned>(Frontlight.brightness()), gpio.isUsbConnected() ? 1 : 0);
}

namespace {
struct WriteNote {
  uint32_t atMs;
  uint32_t offset;
  uint32_t len;
  uint16_t ms;
  bool ok;
};
constexpr size_t WRITE_RING = 24;
WriteNote writes[WRITE_RING];
size_t writeCount = 0;
portMUX_TYPE writeLock = portMUX_INITIALIZER_UNLOCKED;
}  // namespace

void noteWrite(const size_t offset, const size_t len, const uint32_t ms, const bool ok) {
  portENTER_CRITICAL_SAFE(&writeLock);
  writes[writeCount % WRITE_RING] = {static_cast<uint32_t>(millis()), static_cast<uint32_t>(offset),
                                     static_cast<uint32_t>(len), static_cast<uint16_t>(std::min<uint32_t>(ms, 65535)),
                                     ok};
  writeCount++;
  portEXIT_CRITICAL_SAFE(&writeLock);
}

void resetWrites() {
  portENTER_CRITICAL_SAFE(&writeLock);
  writeCount = 0;
  portEXIT_CRITICAL_SAFE(&writeLock);
}

void logWrites(const char* when) {
  WriteNote copy[WRITE_RING];
  portENTER_CRITICAL_SAFE(&writeLock);
  const size_t count = writeCount;
  memcpy(copy, writes, sizeof(copy));
  portEXIT_CRITICAL_SAFE(&writeLock);
  const size_t first = count > WRITE_RING ? count - WRITE_RING : 0;
  LOG_INF("BOOX", "%s: %u SD writes, last %u:", when, static_cast<unsigned>(count),
          static_cast<unsigned>(count - first));
  for (size_t i = first; i < count; ++i) {
    const WriteNote& w = copy[i % WRITE_RING];
    LOG_INF("BOOX", "  write #%u at %u ms: %u bytes at %u took %u ms %s", static_cast<unsigned>(i + 1),
            static_cast<unsigned>(w.atMs), static_cast<unsigned>(w.len), static_cast<unsigned>(w.offset),
            static_cast<unsigned>(w.ms), w.ok ? "ok" : "FAILED");
  }
}

}  // namespace booxdiag

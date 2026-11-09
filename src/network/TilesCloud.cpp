#include "TilesCloud.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <ESPmDNS.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <SDCardManager.h>
#include <SecureHttpClient.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <wolfssl/wolfcrypt/memory.h>
#include <wolfssl/wolfcrypt/settings.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "HttpDownloader.h"
#include "WifiCredentialStore.h"
#include "apps/Wallpaper.h"
#include "util/TaskWatchdog.h"

void addToLogRingBuffer(const char* message);  // Logging.cpp

namespace tilescloud {
namespace {

constexpr const char* LOG_PATH = "/tiles-cloud-log.txt";
// Start a new log instead of growing past this.
constexpr size_t LOG_MAX_BYTES = 256 * 1024;
// Response bytes kept per request.
constexpr size_t BODY_CAP = 6 * 1024;
constexpr const char* NVS_NAMESPACE = "user_cred";
constexpr const char* NVS_USER_INFO = "userInfo";
constexpr const char* API_BASE = "https://push.boox.com/api/v2/nano";
constexpr const char* PUSH_LIST_URL = "https://push.boox.com/api/v2/nano/push/list?limit=10&offset=0";
// Read-only endpoints the stock firmware uses, logged for later work.
constexpr const char* EXTRA_GETS[] = {"/todo", "/countdownDay", "/statistics/summary"};
constexpr const char* PUSH_REPORT_URL = "https://push.boox.com/api/v2/nano/push/downloaded";
// Authorization value that worked, kept for polls during this session.
std::string g_authorization;
// Push items already downloaded this session.
std::vector<std::string> g_downloaded;
Status g_status;
// The stock firmware's User-Agent; the server may expect it.
constexpr const char* STOCK_USER_AGENT = "Picco-ESP32-1.0.5";

std::string efuseHex() {
  char buf[17];
  snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(ESP.getEfuseMac()));
  return buf;
}

std::string macText() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return buf;
}

const char* nvsTypeName(const nvs_type_t type) {
  switch (type) {
    case NVS_TYPE_U8:
      return "u8";
    case NVS_TYPE_I8:
      return "i8";
    case NVS_TYPE_U16:
      return "u16";
    case NVS_TYPE_I16:
      return "i16";
    case NVS_TYPE_U32:
      return "u32";
    case NVS_TYPE_I32:
      return "i32";
    case NVS_TYPE_U64:
      return "u64";
    case NVS_TYPE_I64:
      return "i64";
    case NVS_TYPE_STR:
      return "str";
    case NVS_TYPE_BLOB:
      return "blob";
    default:
      return "?";
  }
}

// Every NVS key with its namespace and type; values are not read.
void logNvsKeys(Print& out) {
  out.println("NVS keys (namespace/key type):");
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
  int count = 0;
  while (err == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    out.printf("  %s/%s %s\n", info.namespace_name, info.key, nvsTypeName(info.type));
    ++count;
    err = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  out.printf("  (%d keys)\n", count);
}

// Read user_cred/userInfo, stored as a string or a blob.
bool readUserInfo(std::string& out, Print& log) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    log.printf("open %s: %s\n", NVS_NAMESPACE, esp_err_to_name(err));
    return false;
  }
  size_t len = 0;
  bool isStr = true;
  err = nvs_get_str(handle, NVS_USER_INFO, nullptr, &len);
  if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
    isStr = false;
    err = nvs_get_blob(handle, NVS_USER_INFO, nullptr, &len);
  }
  if (err != ESP_OK || len == 0) {
    log.printf("read %s/%s: %s\n", NVS_NAMESPACE, NVS_USER_INFO, esp_err_to_name(err));
    nvs_close(handle);
    return false;
  }
  out.resize(len);
  err = isStr ? nvs_get_str(handle, NVS_USER_INFO, out.data(), &len)
              : nvs_get_blob(handle, NVS_USER_INFO, out.data(), &len);
  nvs_close(handle);
  if (err != ESP_OK) {
    log.printf("read %s/%s: %s\n", NVS_NAMESPACE, NVS_USER_INFO, esp_err_to_name(err));
    return false;
  }
  if (isStr && !out.empty() && out.back() == '\0') out.pop_back();
  log.printf("%s/%s: %s, %u bytes\n", NVS_NAMESPACE, NVS_USER_INFO, isStr ? "str" : "blob",
             static_cast<unsigned>(out.size()));
  return true;
}

// Print the JSON layout: keys and value types, string lengths instead of values.
void logJsonShape(Print& out, JsonVariantConst v, const std::string& path, int depth) {
  if (depth > 6) return;
  if (v.is<JsonObjectConst>()) {
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
      logJsonShape(out, kv.value(), path + "." + kv.key().c_str(), depth + 1);
    }
  } else if (v.is<JsonArrayConst>()) {
    out.printf("  %s: array[%u]\n", path.c_str(), static_cast<unsigned>(v.size()));
    if (v.size() > 0) logJsonShape(out, v[0], path + "[0]", depth + 1);
  } else if (v.is<const char*>()) {
    out.printf("  %s: string(%u)\n", path.c_str(), static_cast<unsigned>(strlen(v.as<const char*>())));
  } else if (v.is<bool>()) {
    out.printf("  %s: bool\n", path.c_str());
  } else if (v.isNull()) {
    out.printf("  %s: null\n", path.c_str());
  } else {
    out.printf("  %s: number\n", path.c_str());
  }
}

// First string value named `key` anywhere in the document.
std::string findString(JsonVariantConst v, const char* key, int depth = 0) {
  if (depth > 6) return {};
  if (v.is<JsonObjectConst>()) {
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
      if (strcmp(kv.key().c_str(), key) == 0 && kv.value().is<const char*>()) return kv.value().as<const char*>();
    }
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
      std::string found = findString(kv.value(), key, depth + 1);
      if (!found.empty()) return found;
    }
  } else if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst item : v.as<JsonArrayConst>()) {
      std::string found = findString(item, key, depth + 1);
      if (!found.empty()) return found;
    }
  }
  return {};
}

// GET `url` with an Authorization header and log the status and body.
int logGet(Print& out, const char* url, const std::string& authorization, const char* label,
           std::string* bodyOut = nullptr, const size_t cap = BODY_CAP) {
  freeink::SecureHttpClient http;
  if (!http.begin(url)) {
    out.printf("[%s] bad URL\n", label);
    return 0;
  }
  http.setTimeout(15000);
  http.setInsecure();
  http.setUserAgent(STOCK_USER_AGENT);
  http.addHeader("Authorization", authorization);
  http.addHeader("mac", macText());
  std::string body;
  const int status = http.GET([&](const uint8_t* data, size_t len) {
    if (body.size() < cap) body.append(reinterpret_cast<const char*>(data), std::min(len, cap - body.size()));
    return true;
  });
  http.end();
  out.printf("[%s] GET %s -> %d, %u bytes%s\n", label, url, status, static_cast<unsigned>(body.size()),
             body.size() >= cap ? " (cut)" : "");
  out.println(body.c_str());
  if (bodyOut) *bodyOut = std::move(body);
  return status;
}

// Long string values (tokens, secrets) shortened so the log never holds them.
void redactLongStrings(JsonVariant v, int depth = 0) {
  if (depth > 6) return;
  if (v.is<JsonObject>()) {
    for (JsonPair kv : v.as<JsonObject>()) {
      JsonVariant child = kv.value();
      if (child.is<const char*>()) {
        const std::string text = child.as<const char*>();
        if (text.size() > 24) child.set(text.substr(0, 6) + "...(" + std::to_string(text.size()) + " chars)");
      } else {
        redactLongStrings(child, depth + 1);
      }
    }
  } else if (v.is<JsonArray>()) {
    for (JsonVariant item : v.as<JsonArray>()) redactLongStrings(item, depth + 1);
  }
}

// Ask for a device token the way the stock firmware does before it has one:
// /users/deviceToken with only the User-Agent and "mac" headers, no
// Authorization. Logs the method, status and the reply with secrets cut.
// Returns the token when the server hands one out.
std::string requestDeviceToken(Print& out) {
  const std::string url = std::string(API_BASE) + "/users/deviceToken";
  for (const char* method : {"GET", "POST"}) {
    freeink::SecureHttpClient http;
    if (!http.begin(url.c_str())) return {};
    http.setTimeout(15000);
    http.setInsecure();
    http.setUserAgent(STOCK_USER_AGENT);
    http.addHeader("mac", macText());
    const unsigned long start = millis();
    int status;
    if (strcmp(method, "GET") == 0) {
      status = http.GET();
    } else {
      http.addHeader("Content-Type", "application/json");
      status = http.sendRequest("POST", std::string("{}"));
    }
    std::string body = http.getString();
    http.end();
    if (body.size() > BODY_CAP) body.resize(BODY_CAP);
    out.printf("[deviceToken] %s %s (mac %s) -> %d, %u bytes, %lu ms\n", method, url.c_str(), macText().c_str(), status,
               static_cast<unsigned>(body.size()), millis() - start);
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      out.println(body.substr(0, 1000).c_str());
    } else {
      std::string token = findString(doc.as<JsonVariantConst>(), "token");
      redactLongStrings(doc.as<JsonVariant>());
      serializeJson(doc, out);
      out.println();
      if (!token.empty()) {
        out.printf("[deviceToken] got a token (%u chars) using only the mac header\n",
                   static_cast<unsigned>(token.size()));
        return token;
      }
    }
    // Only try POST when the server says GET is the wrong method.
    if (status != 404 && status != 405) break;
  }
  return {};
}

// File name for a push item: its name-like field, else its id.
std::string pushFileName(JsonObjectConst item) {
  for (const char* key : {"name", "fileName", "filename", "title", "summary"}) {
    const char* v = item[key] | "";
    if (*v) {
      std::string name = v;
      for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|')
          c = '_';
      }
      return name;
    }
  }
  for (const char* key : {"guid", "id", "file_id", "fileId"}) {
    if (item[key].is<const char*>()) return item[key].as<const char*>();
    if (item[key].is<long long>()) return std::to_string(item[key].as<long long>());
  }
  return "push-item";
}

// The item's id as a string, as the stock firmware reports it.
std::string pushItemId(JsonObjectConst item) {
  if (item["id"].is<const char*>()) return item["id"].as<const char*>();
  if (item["id"].is<long long>()) return std::to_string(item["id"].as<long long>());
  return {};
}

// Tell the server which pushed items arrived, as the stock firmware does:
// POST {"ids":[...]} to /push/downloaded.
void reportDownloaded(Print& out, const std::vector<std::string>& ids) {
  if (ids.empty()) return;
  JsonDocument doc;
  JsonArray arr = doc["ids"].to<JsonArray>();
  for (const auto& id : ids) arr.add(id);
  std::string payload;
  serializeJson(doc, payload);
  freeink::SecureHttpClient http;
  if (!http.begin(PUSH_REPORT_URL)) return;
  http.setTimeout(15000);
  http.setInsecure();
  http.setUserAgent(STOCK_USER_AGENT);
  http.addHeader("Authorization", g_authorization);
  http.addHeader("mac", macText());
  http.addHeader("Content-Type", "application/json");
  const int status = http.sendRequest("POST", payload);
  const std::string& reply = http.getString();
  out.printf("report POST %s -> %d: %s\n", payload.c_str(), status, reply.substr(0, 500).c_str());
  http.end();
}

// TLS memory from PSRAM first, as the stock firmware does ("mbedtls
// allocator -> PSRAM"), so TLS buffers don't take the internal RAM the SD
// card and Wi-Fi need. ESP-IDF free() and realloc() accept memory from
// either pool, so switching back and forth is safe.
void* tlsMallocPsram(size_t size) { return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_8BIT); }
void tlsFree(void* ptr) { heap_caps_free(ptr); }
void* tlsReallocPsram(void* ptr, size_t size) {
  return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_8BIT);
}

void setTlsInPsram(const bool on) {
  if (on && ESP.getPsramSize() > 0) {
    wolfSSL_SetAllocators(tlsMallocPsram, tlsFree, tlsReallocPsram);
  } else {
    wolfSSL_SetAllocators(nullptr, nullptr, nullptr);
  }
}

// Write, read back and remove a small file: "ok" or the step that failed.
const char* sdWriteTest() {
  constexpr const char* PATH = "/.tiles-sd-test";
  HalFile f = Storage.open(PATH, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return "open-fail";
  if (f.write(reinterpret_cast<const uint8_t*>("ok"), 2) != 2) return "write-fail";
  f.close();
  if (!Storage.remove(PATH)) return "remove-fail";
  return "ok";
}

// While alive, keeps every log line written during a cloud download (ours,
// and ESP-IDF's from Wi-Fi, HTTP, the SD card driver and the heap) in a
// PSRAM buffer, so the whole story can be written to the card log
// afterwards, even when the card had to be remounted. ESP-IDF errors and
// warnings also go to the device log ring, which survives a restart.
class DiagCapture {
 public:
  DiagCapture() {
    // Allocated once and never freed: another task may still be inside
    // sink() or capture() just after the destructor unhooks them.
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
  ~DiagCapture() {
    g_logDisplayRefresh = false;
    setLogSink(nullptr);
    esp_log_set_vprintf(previous);
    esp_log_level_set("*", previousLevel);
  }
  // The lines kept by the last capture.
  static void dump(Print& out) {
    if (!buffer) {
      out.println("  (no PSRAM for the download log)");
      return;
    }
    out.printf("  download log (%u bytes, %u bytes dropped):\n", static_cast<unsigned>(used),
               static_cast<unsigned>(dropped));
    out.write(reinterpret_cast<const uint8_t*>(buffer), used);
    out.println();
  }

 private:
  static constexpr size_t CAPACITY = 64 * 1024;
  // SD card driver tags, logged in full.
  static constexpr const char* DEBUG_TAGS[] = {"sdmmc_req", "sdmmc_cmd", "sdmmc_common", "sdmmc_sd",
                                               "sdmmc_periph", "sdmmc_io", "sdmmc_init"};
  static inline char* buffer = nullptr;
  static inline size_t used = 0;
  static inline size_t dropped = 0;
  static inline portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
  static inline vprintf_like_t previous = nullptr;
  esp_log_level_t previousLevel = ESP_LOG_ERROR;

  static void append(const char* text, size_t len) {
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
  static void sink(const char* line) { append(line, strlen(line)); }
  static int capture(const char* format, va_list args) {
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
};

// One line with the state of everything a download depends on.
void logSystemState(const char* when) {
  LOG_INF("TILES", "%s: heap %u max %u, DMA %u max %u, PSRAM %u, heap ok %d, RSSI %d, wifi %d, battery %u%%, stack %u",
          when, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
          static_cast<unsigned>(ESP.getFreePsram()), heap_caps_check_integrity_all(true) ? 1 : 0,
          static_cast<int>(WiFi.RSSI()), static_cast<int>(WiFi.status()),
          static_cast<unsigned>(powerManager.getBatteryPercentage()),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

// Card state below the file system: a raw read of sector 0, then a file
// system read and a write test.
void logCardState(const char* when) {
  static uint8_t sector[512];
  FsBlockDeviceInterface* dev = SdMan.rawBlockDevice();
  const bool raw = dev && dev->readSector(0, sector);
  const bool mbr = raw && sector[510] == 0x55 && sector[511] == 0xAA;
  LOG_INF("TILES", "%s: card ready %d, raw sector 0 read %d (signature %d), log exists %d, write test %s", when,
          Storage.ready() ? 1 : 0, raw ? 1 : 0, mbr ? 1 : 0, Storage.exists(LOG_PATH) ? 1 : 0, sdWriteTest());
}

// Result of stockDownload(), kept for the screen and the log.
struct StockDownload {
  bool ok = false;
  bool netOk = false;
  int status = 0;
  int contentLength = -1;
  size_t written = 0;
  unsigned long ms = 0;
  const char* sdResult = "not started";
  char sha256[65] = "";
};

// The stock firmware's download routine, step for step (see handlePushList).
StockDownload stockDownload(const std::string& url, const std::string& dest, const size_t expected) {
  constexpr size_t BUF_SIZE = 32 * 1024;
  StockDownload d;
  const unsigned long started = millis();
  if (!Storage.ready()) {
    d.sdResult = "not ready";
    return d;
  }
  if (expected > 0 && Storage.freeBytes() < expected) {
    d.sdResult = "no space";
    return d;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 60000;
  config.max_redirection_count = 10;
  config.disable_auto_redirect = true;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    d.sdResult = "http init failed";
    return d;
  }
  esp_http_client_set_header(client, "User-Agent", STOCK_USER_AGENT);
  for (int hop = 0; hop <= 10; ++hop) {
    if (esp_http_client_open(client, 0) != ESP_OK) break;
    d.contentLength = static_cast<int>(esp_http_client_fetch_headers(client));
    d.status = esp_http_client_get_status_code(client);
    LOG_INF("TILES", "download status=%d contentLength=%d redirect=%d", d.status, d.contentLength, hop);
    if (d.status < 300 || d.status >= 400) break;
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    esp_http_client_close(client);
  }
  if (d.status < 200 || d.status >= 300) {
    esp_http_client_cleanup(client);
    d.ms = millis() - started;
    d.sdResult = "skipped";
    return d;
  }

  const std::string tmp = dest + ".tmp";
  Storage.remove(tmp.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("TILES", tmp.c_str(), file)) {
    esp_http_client_cleanup(client);
    d.sdResult = "open .tmp failed";
    return d;
  }
  // The stock loop: a 32 KB PSRAM buffer; each read is written at once,
  // the file is flushed every 256 KB, and the task yields 1 tick per read.
  auto* buf = static_cast<uint8_t*>(heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buf) {
    esp_http_client_cleanup(client);
    d.sdResult = "no PSRAM buffer";
    return d;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  bool writeOk = true;
  size_t flushedAt = 0;
  size_t nextMark = 0;
  while (true) {
    const int n = esp_http_client_read(client, reinterpret_cast<char*>(buf), BUF_SIZE);
    if (n < 0) {
      LOG_ERR("TILES", "http read failed errno=%d written=%u", errno, static_cast<unsigned>(d.written));
      break;
    }
    if (n == 0) {
      d.netOk = esp_http_client_is_complete_data_received(client) || d.contentLength <= 0;
      break;
    }
    const unsigned long writeStart = millis();
    const size_t wrote = file.write(buf, n);
    const unsigned long writeMs = millis() - writeStart;
    if (writeMs > 300) {
      LOG_INF("TILES", "slow SD write: %d bytes at %u took %lu ms", n, static_cast<unsigned>(d.written), writeMs);
    }
    if (wrote != static_cast<size_t>(n)) {
      LOG_ERR("TILES", "SD write %u of %d bytes at %u, heap %u, max block %u", static_cast<unsigned>(wrote), n,
              static_cast<unsigned>(d.written), static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
      writeOk = false;
      break;
    }
    mbedtls_sha256_update(&sha, buf, n);
    d.written += n;
    if (d.written - flushedAt >= 256 * 1024) {
      file.flush();
      flushedAt = d.written;
    }
    resetTaskWatchdogIfSubscribed();
    if (d.written >= nextMark) {
      LOG_INF("TILES", "download %u bytes, heap %u", static_cast<unsigned>(d.written),
              static_cast<unsigned>(ESP.getFreeHeap()));
      nextMark += 128 * 1024;  // the ring keeps only 16 lines
    }
    vTaskDelay(1);
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  heap_caps_free(buf);
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  for (int i = 0; i < 32; ++i) snprintf(d.sha256 + i * 2, 3, "%02x", digest[i]);
  // Stock flushes and closes without checking the result; the checks below
  // decide. The result is only logged.
  file.flush();
  const bool closeOk = file.close();
  if (!closeOk) {
    HalFile check = Storage.open(tmp.c_str());
    const unsigned onCard = check ? static_cast<unsigned>(check.size()) : 0u;
    LOG_ERR("TILES", "close returned false; .tmp is %u bytes, wrote %u", onCard, static_cast<unsigned>(d.written));
  }
  d.ms = millis() - started;

  static char sdMsg[48];
  if (!writeOk) {
    snprintf(sdMsg, sizeof(sdMsg), "write failed at %u", static_cast<unsigned>(d.written));
    d.sdResult = sdMsg;
  } else if (!d.netOk) {
    d.sdResult = "incomplete";
  } else if (d.written == 0) {
    d.sdResult = "empty";
  } else if (!Storage.replaceFile(tmp.c_str(), dest.c_str())) {
    d.sdResult = "rename failed";
  } else {
    d.sdResult = "ok";
    d.ok = true;
  }
  if (!d.ok) Storage.remove(tmp.c_str());
  return d;
}

// Log each push item in full, download it to the SD card root from its URL
// and report it as received.
HalFile openLog();

void handlePushList(HalFile& out, const std::string& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  JsonArrayConst list = doc["data"]["list"].as<JsonArrayConst>();
  if (list.isNull()) return;
  g_status.waiting = static_cast<int>(list.size());
  std::vector<std::string> reported;
  for (JsonObjectConst item : list) {
    out.print("push item: ");
    serializeJson(item, out);
    out.println();
    const std::string name = pushFileName(item);
    if (std::find(g_downloaded.begin(), g_downloaded.end(), name) != g_downloaded.end()) continue;
    std::string url = item["url"] | "";
    for (JsonPairConst kv : item) {
      if (!url.empty()) break;
      const char* v = kv.value() | "";
      if (strncmp(v, "http://", 7) == 0 || strncmp(v, "https://", 8) == 0) {
        url = v;
        out.printf("  url field: %s\n", kv.key().c_str());
        break;
      }
    }
    if (url.empty()) {
      out.println("  no URL field; not downloaded");
      continue;
    }
    // Like the stock firmware, pushed .opg images go to the Gallery folder;
    // other files go to the card root as books.
    const bool image = wallpaper::isOpg(name);
    if (image) Storage.mkdir(wallpaper::DIR);
    const std::string dest = (image ? std::string(wallpaper::DIR) + "/" : std::string("/")) + name;
    // The log stays closed while the book is written, so the two files never
    // share the card at once. Progress goes to the device log ring instead.
    size_t nextMark = 0;
    const auto progress = [&nextMark](size_t done, size_t total) {
      resetTaskWatchdogIfSubscribed();
      if (done < nextMark) return;
      nextMark = done + 32 * 1024;
      LOG_INF("TILES", "download %u/%u bytes, heap %u", static_cast<unsigned>(done), static_cast<unsigned>(total),
              static_cast<unsigned>(ESP.getFreeHeap()));
    };
    // Same steps as the stock firmware's downloader: check the card is
    // ready and has room, open "<name>.tmp", read the body with
    // esp_http_client (60 s timeout, up to 10 redirects) into a 32 KB PSRAM
    // buffer, write each read to the file while updating a SHA-256,
    // then rename the file into place. Each step is kept for the screen,
    // since the card log may not be writable after a failure.
    g_status.attemptCount = 0;
    const auto step = [](const char* format, ...) {
      if (g_status.attemptCount >= Status::MAX_ATTEMPTS) return;
      va_list args;
      va_start(args, format);
      vsnprintf(g_status.attempts[g_status.attemptCount++], sizeof(g_status.attempts[0]), format, args);
      va_end(args);
      LOG_INF("TILES", "%s", g_status.attempts[g_status.attemptCount - 1]);
    };
    const uint64_t freeBefore = Storage.freeBytes();
    out.printf("  download %s -> %s (size %u, SD free %llu, heap %u, max block %u, PSRAM free %u)\n", url.c_str(),
               dest.c_str(), static_cast<unsigned>(item["size"] | 0u), static_cast<unsigned long long>(freeBefore),
               static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
               static_cast<unsigned>(ESP.getFreePsram()));
    out.close();
    StockDownload d;
    {
      DiagCapture capture;
      logSystemState("before download");
      logCardState("before download");
      // Like the stock firmware's SD remount retry (max 3): when the card
      // stops answering, remount it and download again.
      constexpr int MAX_TRIES = 3;
      for (int attempt = 1; attempt <= MAX_TRIES; ++attempt) {
        LOG_INF("TILES", "download attempt %d/%d", attempt, MAX_TRIES);
        d = stockDownload(url, dest, item["size"] | 0u);
        logSystemState("after download");
        logCardState("after download");
        if (d.ok || strcmp(sdWriteTest(), "ok") == 0) break;
        Storage.prepareForDeepSleep();
        LOG_ERR("TILES", "card not answering; remount %d", Storage.begin() ? 1 : 0);
        logCardState("after remount");
      }
      if (d.ok) {
        HalFile check = Storage.open(dest.c_str());
        LOG_INF("TILES", "saved %s: %u bytes on card", dest.c_str(), check ? static_cast<unsigned>(check.size()) : 0u);
      }
      step("SD free %lluKB need %uKB", static_cast<unsigned long long>(freeBefore / 1024),
           static_cast<unsigned>((item["size"] | 0u) / 1024));
      step("net %s HTTP %d len %d got %u %lus", d.netOk ? "ok" : "FAIL", d.status, d.contentLength,
           static_cast<unsigned>(d.written), d.ms / 1000);
      step("SD %s", d.sdResult);
      out = openLog();
      if (!out) LOG_ERR("TILES", "cannot reopen %s after download", LOG_PATH);
    }
    DiagCapture::dump(out);
    out.flush();
    LOG_INF("TILES", "log after dump: open %d, %u bytes", out ? 1 : 0, out ? static_cast<unsigned>(out.size()) : 0u);
    const bool gotBook = d.ok;
    for (int i = 0; i < g_status.attemptCount; ++i) out.printf("  %s\n", g_status.attempts[i]);
    out.printf("  %lu ms, sha256 %s, SD free now %llu\n", d.ms, d.sha256,
               static_cast<unsigned long long>(Storage.freeBytes()));
    out.printf("  SD write test now: %s\n", sdWriteTest());
    if (!gotBook) {
      out.println("  recent device log:");
      out.print(getLastLogs().c_str());
      out.println();
    }
    if (!gotBook) g_status.failed++;
    if (gotBook) {
      g_downloaded.push_back(name);
      g_status.received++;
      if (library::isLibraryBookFile(dest.c_str())) library::markLibraryIndexDirty();
      const std::string id = pushItemId(item);
      if (!id.empty()) reported.push_back(id);
    }
  }
  reportDownloaded(out, reported);
}

HalFile openLog() {
  bool fresh = true;
  if (Storage.exists(LOG_PATH)) {
    HalFile existing = Storage.open(LOG_PATH);
    fresh = !existing || existing.size() > LOG_MAX_BYTES;
  }
  return Storage.open(LOG_PATH, fresh ? (O_WRONLY | O_CREAT | O_TRUNC) : (O_WRONLY | O_CREAT | O_APPEND));
}

// The stock firmware's UserAuth asks for a new token once the saved one is
// this old (seconds since tokenLastAt), or when the server rejects it.
constexpr uint32_t TOKEN_FRESH_SECONDS = 14399;

// The saved token and when it was issued (0 when unknown).
std::string savedToken(Print& log, uint32_t& issuedAt) {
  issuedAt = 0;
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
    nvs_get_u32(handle, "tokenLastAt", &issuedAt);
    nvs_close(handle);
  }
  std::string userInfo;
  if (!readUserInfo(userInfo, log)) return {};
  JsonDocument doc;
  if (deserializeJson(doc, userInfo)) return {};
  std::string token = findString(doc.as<JsonVariantConst>(), "accessToken");
  if (token.empty()) token = findString(doc.as<JsonVariantConst>(), "token");
  return token;
}

// The stock firmware's UserAuth steps: GET /users/deviceToken for a new
// token, read /users/me with it, then save both to NVS user_cred (userInfo,
// token, deviceId, secretHex, tokenLastAt). False while the server has no
// token for this device.
bool refreshToken(Print& log) {
  const std::string token = requestDeviceToken(log);
  if (token.empty()) return false;
  std::string body;
  const std::string url = std::string(API_BASE) + "/users/me";
  const int status = logGet(log, url.c_str(), token, "me", &body, 16 * 1024);
  JsonDocument reply;
  if (status != 200 || deserializeJson(reply, body) || (reply["result_code"] | -1) != 0 ||
      !reply["data"].is<JsonObject>()) {
    log.println("auth: /users/me did not return the account");
    return false;
  }
  // The stock userInfo is the /users/me data plus accessToken and tokenExpiredAt.
  JsonDocument info;
  info.set(reply["data"]);
  info["accessToken"] = token;
  if (!reply["tokenExpiredAt"].isNull()) info["tokenExpiredAt"] = reply["tokenExpiredAt"];
  std::string userInfo;
  serializeJson(info, userInfo);
  const std::string deviceId = findString(info.as<JsonVariantConst>(), "id");
  const char* devId = info["device"]["id"] | deviceId.c_str();
  const char* secret = info["device"]["secretHex"] | "";
  const auto now = static_cast<uint32_t>(time(nullptr));

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    err = nvs_set_str(handle, NVS_USER_INFO, userInfo.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, "token", token.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, "deviceId", devId);
    if (err == ESP_OK && *secret) err = nvs_set_str(handle, "secretHex", secret);
    if (err == ESP_OK) err = nvs_set_u32(handle, "tokenLastAt", now);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
  }
  log.printf("auth: saved new token (%u chars, userInfo %u bytes, device %s, secret %u chars, time %u): %s\n",
             static_cast<unsigned>(token.size()), static_cast<unsigned>(userInfo.size()), devId,
             static_cast<unsigned>(strlen(secret)), static_cast<unsigned>(now), esp_err_to_name(err));
  g_authorization = token;
  g_status.bound = true;
  return true;
}

// Token for cloud requests, as the stock UserAuth picks it: the saved token
// while it is fresh and the clock is set, else a new one from
// /users/deviceToken. `force` asks for a new one, after a 401.
bool ensureAuthorization(Print& log, const bool force = false) {
  uint32_t issuedAt = 0;
  const std::string saved = savedToken(log, issuedAt);
  const auto now = static_cast<uint32_t>(time(nullptr));
  const bool clockSet = now > 1000000000u;
  log.printf("auth: saved token %u chars, issued %u, now %u, force %d\n", static_cast<unsigned>(saved.size()),
             static_cast<unsigned>(issuedAt), static_cast<unsigned>(now), force);
  if (!force && !saved.empty() && clockSet && issuedAt != 0 && now >= issuedAt &&
      now - issuedAt <= TOKEN_FRESH_SECONDS) {
    log.println("auth: saved token still fresh");
    g_authorization = saved;
    g_status.bound = true;
    return true;
  }
  if (refreshToken(log)) return true;
  log.println("auth: no new token from /users/deviceToken");
  if (force || saved.empty()) return false;
  g_authorization = saved;
  g_status.bound = true;
  return true;
}

// GET with the current token; on 401 or 403 get a new token once and retry,
// as the stock firmware does when the server says the token expired.
int authedGet(Print& log, const char* url, const char* label, std::string* bodyOut, const size_t cap = BODY_CAP) {
  int status = logGet(log, url, g_authorization, label, bodyOut, cap);
  if ((status == 401 || status == 403) && ensureAuthorization(log, true)) {
    status = logGet(log, url, g_authorization, label, bodyOut, cap);
  }
  return status;
}

// Join the last used Wi-Fi network, as the stock apps do before a cloud
// sync. `joined` is set when this call turned Wi-Fi on.
bool ensureWifi(Print& log, bool& joined) {
  joined = false;
  if (WiFi.status() == WL_CONNECTED) return true;
  WIFI_STORE.loadFromFile();
  const auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred) {
    log.println("no saved Wi-Fi network; keep cached list");
    return false;
  }
  joined = true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(100);
  const bool ok = WiFi.status() == WL_CONNECTED;
  log.printf("Wi-Fi %s: %s after %lu ms\n", cred->ssid.c_str(), ok ? "connected" : "failed", millis() - start);
  if (ok) trustedtime::startSync();
  return ok;
}

void stopWifi(const bool joined) {
  if (!joined) return;
  // The Picco has PSRAM, so Wi-Fi turns off in place, as in the Weather app.
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

// GET API_BASE + path and return the reply's data.list, as the stock
// firmware's list request does ({result_code, message, data:{total, limit,
// offset, list}}). A rejected token is replaced once.
// Set when the cloud refused the token and handed out no new one: the
// device is no longer bound to an account.
bool g_unbound = false;

bool fetchList(Print& log, const char* path, JsonDocument& doc, JsonArrayConst& list) {
  g_unbound = false;
  const std::string url = std::string(API_BASE) + path;
  std::string body;
  const int status = authedGet(log, url.c_str(), "sync", &body, 32 * 1024);
  if (status == 401 || status == 403) {
    g_unbound = true;
    log.println("token refused and no new one: device is not bound; bind it again");
  }
  if (status != 200) return false;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    log.printf("JSON parse failed: %s\n", err.c_str());
    return false;
  }
  const int code = doc["result_code"] | -1;
  list = doc["data"]["list"].as<JsonArrayConst>();
  log.printf("result_code %d, total %d, list %s with %u items\n", code, doc["data"]["total"] | -1,
             list.isNull() ? "missing" : "present", static_cast<unsigned>(list.size()));
  return code == 0 && !list.isNull();
}

std::string idOf(JsonObjectConst item) {
  for (const char* key : {"_id", "id"}) {
    if (item[key].is<const char*>()) return item[key].as<const char*>();
    if (item[key].is<long long>()) return std::to_string(item[key].as<long long>());
  }
  return {};
}

bool truthy(JsonVariantConst v) {
  if (v.is<bool>()) return v.as<bool>();
  if (v.is<long long>()) return v.as<long long>() != 0;
  if (v.is<const char*>()) return strcmp(v.as<const char*>(), "true") == 0 || strcmp(v.as<const char*>(), "1") == 0;
  return false;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
long daysFromCivil(int y, const unsigned m, const unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  return static_cast<long>(era) * 146097L + static_cast<long>(yoe * 365u + yoe / 4u - yoe / 100u + doy) - 719468L;
}

// The countdown's "date": epoch seconds or milliseconds, "YYYY-MM-DD", or an
// ISO time in UTC ("...Z"), which is moved to the local date.
bool cloudDate(JsonVariantConst v, applists::CountdownItem& item) {
  long long epoch = 0;
  bool haveEpoch = false;
  if (v.is<long long>() || v.is<double>()) {
    epoch = v.as<long long>();
    haveEpoch = true;
  } else if (v.is<const char*>()) {
    const char* text = v.as<const char*>();
    int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
    const int n = sscanf(text, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &hh, &mm, &ss);
    if (n >= 3) {
      if (y < 1970 || y > 2200 || m < 1 || m > 12 || d < 1 || d > 31) return false;
      if (n < 6 || !strchr(text, 'Z')) {
        item.year = static_cast<int16_t>(y);
        item.month = static_cast<uint8_t>(m);
        item.day = static_cast<uint8_t>(d);
        return true;
      }
      epoch = daysFromCivil(y, m, d) * 86400LL + hh * 3600LL + mm * 60LL + ss;
      haveEpoch = true;
    } else {
      char* end = nullptr;
      epoch = strtoll(text, &end, 10);
      haveEpoch = end != text;
    }
  }
  if (!haveEpoch || epoch <= 0) return false;
  if (epoch > 100000000000LL) epoch /= 1000;
  const auto t = static_cast<time_t>(epoch);
  struct tm local;
  localtime_r(&t, &local);
  item.year = static_cast<int16_t>(local.tm_year + 1900);
  item.month = static_cast<uint8_t>(local.tm_mon + 1);
  item.day = static_cast<uint8_t>(local.tm_mday);
  return true;
}

std::string clip(const char* text) {
  std::string out = text ? text : "";
  if (out.size() <= applists::MAX_TEXT) return out;
  size_t cut = applists::MAX_TEXT;
  while (cut > 0 && (static_cast<uint8_t>(out[cut]) & 0xC0) == 0x80) --cut;
  out.resize(cut);
  return out;
}

// Print that writes each finished line to the log file at once, so a sync
// that hangs still leaves its last step in the log.
class LineLog final : public Print {
 public:
  ~LineLog() override { flushLine(); }
  size_t write(const uint8_t c) override {
    if (c == '\n') {
      flushLine();
    } else if (line.size() < 1024) {
      line.push_back(static_cast<char>(c));
    }
    return 1;
  }
  using Print::write;

 private:
  void flushLine() {
    if (line.empty()) return;
    HalFile log = openLog();
    if (log) {
      log.println(line.c_str());
      log.close();
    }
    line.clear();
  }
  std::string line;
};

// Shared steps of syncTodos and syncCountdowns; `parse` turns the list into items.
template <typename Parse>
SyncResult syncList(const char* label, const char* path, Parse parse) {
  if (!BoardConfig::isPicco()) return SyncResult::Skipped;
  LineLog log;
  log.printf("\n--- %s sync, uptime %lu s, heap %u\n", label, millis() / 1000,
             static_cast<unsigned>(ESP.getFreeHeap()));
  bool joined = false;
  log.println("joining Wi-Fi");
  if (!ensureWifi(log, joined)) {
    stopWifi(joined);
    return SyncResult::NoWifi;
  }
  setTlsInPsram(true);
  log.printf("requesting %s\n", path);
  SyncResult result = SyncResult::Failed;
  if (!ensureAuthorization(log)) {
    log.println("no binding token; skip cloud sync");
    result = SyncResult::NoBinding;
  } else {
    JsonDocument doc;
    JsonArrayConst list;
    if (fetchList(log, path, doc, list)) {
      parse(list, log);
      result = SyncResult::Ok;
    } else if (g_unbound) {
      result = SyncResult::NoBinding;
    }
  }
  stopWifi(joined);
  log.printf("%s sync done: %d, %lu s\n", label, static_cast<int>(result), millis() / 1000);
  return result;
}

}  // namespace

void logLine(const char* format, ...) {
  if (!BoardConfig::isPicco()) return;
  char line[256];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  HalFile log = openLog();
  if (!log) return;
  log.printf("[%lu s] %s\n", millis() / 1000, line);
  log.close();
}

void logBoot(const int resetReason, const std::string& previousLogs) {
  if (!BoardConfig::isPicco()) return;
  HalFile log = openLog();
  if (!log) return;
  log.printf("\n### boot %s, reset reason %d\n", CROSSPOINT_VERSION, resetReason);
  if (!previousLogs.empty()) {
    log.println("device log before the reset:");
    log.print(previousLogs.c_str());
    log.println();
  }
  log.close();
}

void logRecentDeviceLog(const char* why) {
  if (!BoardConfig::isPicco()) return;
  const std::string recent = getLastLogs();
  HalFile log = openLog();
  if (!log) return;
  log.printf("[%lu s] recent device log (%s), heap %u, max block %u:\n", millis() / 1000, why,
             static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  log.print(recent.c_str());
  log.println();
  log.close();
}

std::string mdnsHostname(const char* fallback) {
  if (!BoardConfig::isPicco()) return fallback;
  return "Picco-" + efuseHex();
}

void addMdnsService() {
  if (!BoardConfig::isPicco()) return;
  MDNS.addService("nanoreader", "tcp", 80);
  MDNS.addServiceTxt("nanoreader", "tcp", "MAC", macText().c_str());
  MDNS.addServiceTxt("nanoreader", "tcp", "uniqueId", efuseHex().c_str());
}

void probe() {
  if (!BoardConfig::isPicco()) return;

  HalFile log = openLog();
  if (!log) {
    LOG_ERR("TILES", "cannot open %s", LOG_PATH);
    return;
  }
  log.printf("\n=== %s, uptime %lu s, heap %u ===\n", CROSSPOINT_VERSION, millis() / 1000,
             static_cast<unsigned>(ESP.getFreeHeap()));
  log.printf("mDNS name %s, MAC %s\n", mdnsHostname("").c_str(), macText().c_str());
  // A crash during the last download shows up here as PANIC or TASK_WDT.
  log.printf("last reset reason %d, max block %u, PSRAM free %u\n", static_cast<int>(esp_reset_reason()),
             static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(ESP.getFreePsram()));
  logNvsKeys(log);
  setTlsInPsram(true);
  std::string userInfo;
  if (readUserInfo(userInfo, log)) {
    JsonDocument doc;
    const DeserializationError jsonErr = deserializeJson(doc, userInfo);
    if (jsonErr) {
      log.printf("userInfo is not JSON: %s\n", jsonErr.c_str());
    } else {
      log.println("userInfo layout:");
      logJsonShape(log, doc.as<JsonVariantConst>(), "", 0);
    }
    userInfo.clear();
  } else {
    log.println("No saved binding found.");
  }
  if (!ensureAuthorization(log)) {
    log.println("No token: bind the device first.");
    log.close();
    return;
  }
  log.flush();

  std::string body;
  const int status = authedGet(log, PUSH_LIST_URL, "push list", &body);
  g_status.lastHttp = status;
  if (status == 200) handlePushList(log, body);
  for (const char* path : EXTRA_GETS) {
    const std::string url = std::string(API_BASE) + path;
    authedGet(log, url.c_str(), "extra", nullptr);
  }
  log.close();
  LOG_INF("TILES", "probe written to %s", LOG_PATH);
}

const Status& status() { return g_status; }

bool finishBinding() {
  if (!BoardConfig::isPicco()) return false;
  HalFile log = openLog();
  if (!log) return false;
  setTlsInPsram(true);
  const bool ok = refreshToken(log);
  log.close();
  return ok;
}

SyncResult syncTodos(std::vector<applists::TodoItem>& items) {
  return syncList("todo", "/todo", [&items](JsonArrayConst list, Print& log) {
    std::vector<applists::TodoItem> cloud;
    for (JsonObjectConst entry : list) {
      applists::TodoItem item;
      item.text = clip(entry["content"] | "");
      item.done = truthy(entry["finished"]);
      item.cloudId = idOf(entry);
      log.printf("  todo id=%s finished=%d status=%d len=%u\n", item.cloudId.c_str(), item.done, entry["status"] | -1,
                 static_cast<unsigned>(item.text.size()));
      if (!item.text.empty() && cloud.size() < applists::MAX_TODOS) cloud.push_back(std::move(item));
    }
    applists::mergeCloudTodos(items, std::move(cloud));
    log.printf("todo list now %u items\n", static_cast<unsigned>(items.size()));
  });
}

SyncResult syncCountdowns(std::vector<applists::CountdownItem>& items) {
  return syncList("countdown", "/countdownDay", [&items](JsonArrayConst list, Print& log) {
    std::vector<applists::CountdownItem> cloud;
    for (JsonObjectConst entry : list) {
      applists::CountdownItem item;
      item.title = clip(entry["title"] | "");
      item.cloudId = idOf(entry);
      const bool dated = cloudDate(entry["date"], item);
      std::string rawDate;
      serializeJson(entry["date"], rawDate);
      log.printf("  countdown id=%s date=%s -> %04d-%02d-%02d status=%d\n", item.cloudId.c_str(), rawDate.c_str(),
                 item.year, item.month, item.day, entry["status"] | -1);
      if (!item.title.empty() && dated && cloud.size() < applists::MAX_COUNTDOWNS) cloud.push_back(std::move(item));
    }
    applists::mergeCloudCountdowns(items, std::move(cloud));
    log.printf("countdown list now %u items\n", static_cast<unsigned>(items.size()));
  });
}

void poll() {
  if (!BoardConfig::isPicco() || g_authorization.empty()) return;
  HalFile log = openLog();
  if (!log) return;
  log.printf("\n--- poll, uptime %lu s, heap %u\n", millis() / 1000, static_cast<unsigned>(ESP.getFreeHeap()));
  std::string body;
  g_status.lastHttp = authedGet(log, PUSH_LIST_URL, "poll", &body);
  if (g_status.lastHttp == 200) handlePushList(log, body);
  log.close();
}

}  // namespace tilescloud

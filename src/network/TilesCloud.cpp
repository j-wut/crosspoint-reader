#include "TilesCloud.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <ESPmDNS.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <esp_mac.h>
#include <nvs.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "HttpDownloader.h"

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
           std::string* bodyOut = nullptr) {
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
    if (body.size() < BODY_CAP) body.append(reinterpret_cast<const char*>(data), std::min(len, BODY_CAP - body.size()));
    return true;
  });
  http.end();
  out.printf("[%s] GET %s -> %d, %u bytes%s\n", label, url, status, static_cast<unsigned>(body.size()),
             body.size() >= BODY_CAP ? " (cut)" : "");
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
    const std::string dest = "/" + name;
    // The log stays closed while the book is written, so the two files never
    // share the card at once. Progress goes to the device log ring instead.
    size_t nextMark = 0;
    const auto progress = [&nextMark](size_t done, size_t total) {
      if (done < nextMark) return;
      nextMark = done + 32 * 1024;
      LOG_INF("TILES", "download %u/%u bytes, heap %u", static_cast<unsigned>(done), static_cast<unsigned>(total),
              static_cast<unsigned>(ESP.getFreeHeap()));
    };
    const auto attempt = [&](const std::string& from) {
      out.printf("  download %s -> %s (size %u, heap %u, max block %u)\n", from.c_str(), dest.c_str(),
                 static_cast<unsigned>(item["size"] | 0u), static_cast<unsigned>(ESP.getFreeHeap()),
                 static_cast<unsigned>(ESP.getMaxAllocHeap()));
      out.close();
      nextMark = 0;
      const unsigned long started = millis();
      auto r = HttpDownloader::downloadToFile(from, dest, progress);
      if (r == HttpDownloader::UNAUTHORIZED) {
        r = HttpDownloader::downloadToFile(from, dest, progress, nullptr, "", "", {{"Authorization", g_authorization}});
      }
      const unsigned long elapsed = millis() - started;
      out = openLog();
      out.printf("  -> result %d, HTTP %d, %lu ms, part file %s\n", static_cast<int>(r), HttpDownloader::lastStatus(),
                 elapsed, Storage.exists((dest + ".part").c_str()) ? "left" : "gone");
      return r;
    };
    // The push list gives plain-http storage links; try the same link over
    // https first, then the link as given.
    auto result = HttpDownloader::HTTP_ERROR;
    if (url.rfind("http://", 0) == 0) result = attempt("https://" + url.substr(7));
    if (result != HttpDownloader::OK) result = attempt(url);
    if (result != HttpDownloader::OK) {
      out.println("  recent device log:");
      out.print(getLastLogs().c_str());
      out.println();
    }
    if (result != HttpDownloader::OK) g_status.failed++;
    if (result == HttpDownloader::OK) {
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
  logNvsKeys(log);
  // Tests whether the server links the device by its MAC alone, which is
  // what binding from this firmware would rely on.
  std::string macToken = requestDeviceToken(log);
  log.flush();

  std::string token;
  std::string userInfo;
  if (readUserInfo(userInfo, log)) {
    JsonDocument doc;
    const DeserializationError jsonErr = deserializeJson(doc, userInfo);
    if (jsonErr) {
      log.printf("userInfo is not JSON: %s\n", jsonErr.c_str());
    } else {
      log.println("userInfo layout:");
      logJsonShape(log, doc.as<JsonVariantConst>(), "", 0);
      token = findString(doc.as<JsonVariantConst>(), "accessToken");
      if (token.empty()) token = findString(doc.as<JsonVariantConst>(), "token");
    }
    userInfo.clear();
  } else {
    log.println("No saved binding found.");
  }
  if (token.empty() && !macToken.empty()) {
    log.println("Using the token from /users/deviceToken.");
    token = macToken;
  }
  if (token.empty()) {
    log.println("No token in userInfo.");
    log.close();
    return;
  }
  log.printf("token: %u chars\n", static_cast<unsigned>(token.size()));
  g_status.bound = true;
  log.flush();

  // The stock firmware's header format is unknown: try the raw token, then Bearer.
  std::string body;
  g_authorization = token;
  int status = logGet(log, PUSH_LIST_URL, g_authorization, "raw", &body);
  if (status == 401 || status == 403) {
    g_authorization = "Bearer " + token;
    status = logGet(log, PUSH_LIST_URL, g_authorization, "bearer", &body);
  }
  g_status.lastHttp = status;
  if (status == 200) handlePushList(log, body);
  for (const char* path : EXTRA_GETS) {
    const std::string url = std::string(API_BASE) + path;
    logGet(log, url.c_str(), g_authorization, "extra");
  }
  log.close();
  LOG_INF("TILES", "probe written to %s", LOG_PATH);
}

const Status& status() { return g_status; }

void poll() {
  if (!BoardConfig::isPicco() || g_authorization.empty()) return;
  HalFile log = openLog();
  if (!log) return;
  log.printf("\n--- poll, uptime %lu s, heap %u\n", millis() / 1000, static_cast<unsigned>(ESP.getFreeHeap()));
  std::string body;
  g_status.lastHttp = logGet(log, PUSH_LIST_URL, g_authorization, "poll", &body);
  if (g_status.lastHttp == 200) handlePushList(log, body);
  log.close();
}

}  // namespace tilescloud

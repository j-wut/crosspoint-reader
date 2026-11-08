#include "TilesCloud.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <esp_mac.h>
#include <nvs.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tilescloud {
namespace {

constexpr const char* LOG_PATH = "/tiles-cloud-log.txt";
// Start a new log instead of growing past this.
constexpr size_t LOG_MAX_BYTES = 64 * 1024;
// Response bytes kept per request.
constexpr size_t BODY_CAP = 6 * 1024;
constexpr const char* NVS_NAMESPACE = "user_cred";
constexpr const char* NVS_USER_INFO = "userInfo";
constexpr const char* PUSH_LIST_URL = "https://push.boox.com/api/v2/nano/push/list?limit=10&offset=0";
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
int logGet(Print& out, const char* url, const std::string& authorization, const char* label) {
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
  return status;
}

}  // namespace

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

  const bool fresh = !Storage.exists(LOG_PATH) || Storage.open(LOG_PATH).size() > LOG_MAX_BYTES;
  HalFile log = Storage.open(LOG_PATH, fresh ? (O_WRONLY | O_CREAT | O_TRUNC) : (O_WRONLY | O_CREAT | O_APPEND));
  if (!log) {
    LOG_ERR("TILES", "cannot open %s", LOG_PATH);
    return;
  }
  log.printf("\n=== %s, uptime %lu s, heap %u ===\n", CROSSPOINT_VERSION, millis() / 1000,
             static_cast<unsigned>(ESP.getFreeHeap()));
  log.printf("mDNS name %s, MAC %s\n", mdnsHostname("").c_str(), macText().c_str());
  logNvsKeys(log);

  std::string userInfo;
  if (!readUserInfo(userInfo, log)) {
    log.println("No saved binding found.");
    log.close();
    return;
  }
  JsonDocument doc;
  const DeserializationError jsonErr = deserializeJson(doc, userInfo);
  if (jsonErr) {
    log.printf("userInfo is not JSON: %s\n", jsonErr.c_str());
    log.close();
    return;
  }
  log.println("userInfo layout:");
  logJsonShape(log, doc.as<JsonVariantConst>(), "", 0);

  std::string token = findString(doc.as<JsonVariantConst>(), "accessToken");
  if (token.empty()) token = findString(doc.as<JsonVariantConst>(), "token");
  doc.clear();
  userInfo.clear();
  if (token.empty()) {
    log.println("No token in userInfo.");
    log.close();
    return;
  }
  log.printf("token: %u chars\n", static_cast<unsigned>(token.size()));
  log.flush();

  // The stock firmware's header format is unknown: try the raw token, then Bearer.
  const int status = logGet(log, PUSH_LIST_URL, token, "raw");
  if (status == 401 || status == 403) logGet(log, PUSH_LIST_URL, "Bearer " + token, "bearer");
  log.close();
  LOG_INF("TILES", "probe written to %s", LOG_PATH);
}

}  // namespace tilescloud

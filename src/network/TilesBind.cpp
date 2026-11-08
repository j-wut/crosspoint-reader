#include "TilesBind.h"

#include <Arduino.h>
#include <BlePageTurner.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/sha256.h>

#include <cstdarg>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#include "TilesCloud.h"

namespace tilesbind {
namespace {

// Service and characteristic UUIDs from the stock firmware. Each
// characteristic is the service UUID with bytes 12-13 replaced, and carries
// its endpoint name in a 0x2901 user description, as Espressif's
// protocomm_nimble does.
constexpr const char* SERVICE_UUID = "021a9004-0382-4aea-bff4-6b3f1c5adfb4";
struct Endpoint {
  const char* uuid;
  const char* name;
};
constexpr Endpoint ENDPOINTS[] = {
    {"021aff4f-0382-4aea-bff4-6b3f1c5adfb4", "prov-ctrl"},    {"021aff50-0382-4aea-bff4-6b3f1c5adfb4", "prov-scan"},
    {"021aff51-0382-4aea-bff4-6b3f1c5adfb4", "prov-session"}, {"021aff52-0382-4aea-bff4-6b3f1c5adfb4", "prov-config"},
    {"021aff53-0382-4aea-bff4-6b3f1c5adfb4", "proto-ver"},
};
constexpr const char* VERSION_JSON = R"({"prov":{"ver":"netprov-v1.2","sec_ver":1,"cap":["wifi_prov","wifi_scan"]}})";
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr size_t MAX_SCAN_RESULTS = 20;

// ---- log queue (the BLE host task must not wait on the SD card) ----
std::mutex g_logMutex;
std::vector<std::string> g_logQueue;

void logf(const char* format, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* format, ...) {
  char line[200];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  LOG_INF("BIND", "%s", line);
  std::lock_guard<std::mutex> lock(g_logMutex);
  if (g_logQueue.size() < 200) g_logQueue.emplace_back(line);
}

// ---- protobuf (proto3 wire format, only what these messages need) ----
struct PbWriter {
  std::string out;
  void varint(uint64_t v) {
    while (v >= 0x80) {
      out.push_back(static_cast<char>((v & 0x7F) | 0x80));
      v >>= 7;
    }
    out.push_back(static_cast<char>(v));
  }
  void tag(const uint32_t field, const uint32_t wire) { varint((field << 3) | wire); }
  void u(const uint32_t field, const uint64_t v) {
    tag(field, 0);
    varint(v);
  }
  // int32 fields encode negatives as 10-byte two's complement varints.
  void i32(const uint32_t field, const int32_t v) {
    tag(field, 0);
    varint(static_cast<uint64_t>(static_cast<int64_t>(v)));
  }
  void bytes(const uint32_t field, const void* data, const size_t len) {
    tag(field, 2);
    varint(len);
    out.append(static_cast<const char*>(data), len);
  }
  void sub(const uint32_t field, const std::string& msg) { bytes(field, msg.data(), msg.size()); }
};

struct PbField {
  uint32_t field = 0;
  uint32_t wire = 0;
  uint64_t value = 0;
  const uint8_t* data = nullptr;
  size_t len = 0;
};

struct PbReader {
  const uint8_t* p;
  const uint8_t* end;
  PbReader(const uint8_t* data, const size_t len) : p(data), end(data + len) {}
  explicit PbReader(const PbField& f) : p(f.data), end(f.data + f.len) {}
  bool readVarint(uint64_t& v) {
    v = 0;
    for (int shift = 0; p < end && shift < 64; shift += 7) {
      const uint8_t b = *p++;
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) return true;
    }
    return false;
  }
  bool next(PbField& f) {
    if (p >= end) return false;
    uint64_t key;
    if (!readVarint(key)) return false;
    f = PbField{};
    f.field = static_cast<uint32_t>(key >> 3);
    f.wire = static_cast<uint32_t>(key & 7);
    switch (f.wire) {
      case 0:
        return readVarint(f.value);
      case 1:
        if (end - p < 8) return false;
        p += 8;
        return true;
      case 2: {
        uint64_t len;
        if (!readVarint(len) || len > static_cast<uint64_t>(end - p)) return false;
        f.data = p;
        f.len = static_cast<size_t>(len);
        p += len;
        return true;
      }
      case 5:
        if (end - p < 4) return false;
        p += 4;
        return true;
      default:
        return false;
    }
  }
};

// Find a field in a message; false when absent.
bool findField(const uint8_t* data, const size_t len, const uint32_t field, PbField& out) {
  PbReader r(data, len);
  PbField f;
  while (r.next(f)) {
    if (f.field == field) {
      out = f;
      return true;
    }
  }
  return false;
}

bool findField(const PbField& msg, const uint32_t field, PbField& out) {
  return findField(msg.data, msg.len, field, out);
}

uint64_t fieldValue(const uint8_t* data, const size_t len, const uint32_t field) {
  PbField f;
  return findField(data, len, field, f) ? f.value : 0;
}

// ---- security 1 session (Espressif protocomm security1.c) ----
int randomBytes(void*, unsigned char* out, const size_t len) {
  esp_fill_random(out, len);
  return 0;
}

void flipEndian(uint8_t* data, const size_t len) {
  for (size_t i = 0; i < len / 2; ++i) std::swap(data[i], data[len - 1 - i]);
}

struct Session {
  bool cmd0Done = false;
  bool ready = false;
  uint8_t devicePub[32] = {};
  uint8_t clientPub[32] = {};
  uint8_t key[32] = {};
  uint8_t rand[16] = {};
  uint8_t stb[16] = {};
  size_t ncOff = 0;
  mbedtls_aes_context aes;
  bool aesInit = false;

  void reset() {
    if (aesInit) mbedtls_aes_free(&aes);
    aesInit = false;
    cmd0Done = ready = false;
    ncOff = 0;
    memset(stb, 0, sizeof(stb));
    memset(key, 0, sizeof(key));
  }

  // AES-256-CTR on the session's single running stream: requests are
  // decrypted and replies encrypted in order, as in security1.c.
  std::string crypt(const uint8_t* in, const size_t len) {
    std::string out(len, '\0');
    if (len > 0) {
      mbedtls_aes_crypt_ctr(&aes, len, &ncOff, rand, stb, in, reinterpret_cast<uint8_t*>(out.data()));
    }
    return out;
  }
};

Session g_session;
std::string g_pop;
std::mutex g_stateMutex;
Stage g_stage = Stage::Off;
std::string g_ssid;
std::string g_pass;
bool g_applied = false;
unsigned long g_appliedAt = 0;
NimBLEServer* g_server = nullptr;

void setStage(const Stage s) {
  std::lock_guard<std::mutex> lock(g_stateMutex);
  g_stage = s;
}

// Session command 0: make a Curve25519 key pair, derive the AES key from
// the shared secret XOR SHA-256(pop), and send our public key and random.
std::string sessionCmd0(const PbField& sc0) {
  PbField clientPubField;
  if (!findField(sc0, 1, clientPubField) || clientPubField.len != 32) {
    logf("session cmd0: bad client key (%u bytes)", static_cast<unsigned>(clientPubField.len));
    return {};
  }
  g_session.reset();
  memcpy(g_session.clientPub, clientPubField.data, 32);

  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_mpi z;
  mbedtls_ecp_point Q;
  mbedtls_ecp_point Qp;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&z);
  mbedtls_ecp_point_init(&Q);
  mbedtls_ecp_point_init(&Qp);
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
            mbedtls_ecdh_gen_public(&grp, &d, &Q, randomBytes, nullptr) == 0 &&
            mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(X), g_session.devicePub, 32) == 0;
  flipEndian(g_session.devicePub, 32);
  uint8_t peer[32];
  memcpy(peer, g_session.clientPub, 32);
  flipEndian(peer, 32);
  ok = ok && mbedtls_mpi_lset(&Qp.MBEDTLS_PRIVATE(Z), 1) == 0 &&
       mbedtls_mpi_read_binary(&Qp.MBEDTLS_PRIVATE(X), peer, 32) == 0 &&
       mbedtls_ecdh_compute_shared(&grp, &z, &Qp, &d, randomBytes, nullptr) == 0 &&
       mbedtls_mpi_write_binary(&z, g_session.key, 32) == 0;
  flipEndian(g_session.key, 32);
  mbedtls_ecp_point_free(&Qp);
  mbedtls_ecp_point_free(&Q);
  mbedtls_mpi_free(&z);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  if (!ok) {
    logf("session cmd0: key exchange failed");
    return {};
  }
  uint8_t popHash[32];
  mbedtls_sha256(reinterpret_cast<const uint8_t*>(g_pop.data()), g_pop.size(), popHash, 0);
  for (int i = 0; i < 32; ++i) g_session.key[i] ^= popHash[i];
  esp_fill_random(g_session.rand, sizeof(g_session.rand));
  g_session.cmd0Done = true;

  PbWriter sr0;
  sr0.u(1, 0);  // Success
  sr0.bytes(2, g_session.devicePub, 32);
  sr0.bytes(3, g_session.rand, 16);
  PbWriter sec1;
  sec1.u(1, 1);  // Session_Response0
  sec1.sub(21, sr0.out);
  PbWriter resp;
  resp.u(2, 1);  // SecScheme1
  resp.sub(11, sec1.out);
  logf("session cmd0 ok");
  return resp.out;
}

// Session command 1: the client proves it knows the pop by sending our
// public key encrypted; we answer with its key encrypted.
std::string sessionCmd1(const PbField& sc1) {
  PbField verify;
  if (!g_session.cmd0Done || !findField(sc1, 2, verify) || verify.len != 32) {
    logf("session cmd1: out of order or bad verifier");
    return {};
  }
  mbedtls_aes_init(&g_session.aes);
  g_session.aesInit = true;
  memset(g_session.stb, 0, sizeof(g_session.stb));
  g_session.ncOff = 0;
  if (mbedtls_aes_setkey_enc(&g_session.aes, g_session.key, 256) != 0) {
    logf("session cmd1: AES key failed");
    return {};
  }
  const std::string check = g_session.crypt(verify.data, 32);
  if (memcmp(check.data(), g_session.devicePub, 32) != 0) {
    logf("session cmd1: verifier mismatch (wrong pop?)");
    g_session.reset();
    return {};
  }
  const std::string deviceVerify = g_session.crypt(g_session.clientPub, 32);
  g_session.ready = true;
  setStage(Stage::SessionReady);

  PbWriter sr1;
  sr1.u(1, 0);
  sr1.bytes(3, deviceVerify.data(), deviceVerify.size());
  PbWriter sec1;
  sec1.u(1, 3);  // Session_Response1
  sec1.sub(23, sr1.out);
  PbWriter resp;
  resp.u(2, 1);
  resp.sub(11, sec1.out);
  logf("SESSION_SETUP_OK (waiting for Wi-Fi CRED)");
  return resp.out;
}

std::string handleSession(const std::string& req) {
  const auto* data = reinterpret_cast<const uint8_t*>(req.data());
  PbField sec1;
  if (!findField(data, req.size(), 11, sec1)) {
    logf("session: no sec1 payload (sec_ver %u)", static_cast<unsigned>(fieldValue(data, req.size(), 2)));
    return {};
  }
  PbField payload;
  if (findField(sec1, 20, payload)) return sessionCmd0(payload);
  if (findField(sec1, 22, payload)) return sessionCmd1(payload);
  logf("session: unknown message %u", static_cast<unsigned>(fieldValue(sec1.data, sec1.len, 1)));
  return {};
}

// ---- prov-config ----
uint32_t authModeFor(const wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN:
      return 0;
    case WIFI_AUTH_WEP:
      return 1;
    case WIFI_AUTH_WPA_PSK:
      return 2;
    case WIFI_AUTH_WPA2_PSK:
      return 3;
    case WIFI_AUTH_WPA_WPA2_PSK:
      return 4;
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return 5;
    case WIFI_AUTH_WPA3_PSK:
      return 6;
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return 7;
    default:
      return 3;
  }
}

std::string wifiStatus() {
  PbWriter st;
  st.u(1, 0);
  const wl_status_t wl = WiFi.status();
  if (!g_applied) {
    st.u(2, 2);  // Disconnected
  } else if (wl == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    // As on the stock firmware, "connected" only once there is an IP address.
    st.u(2, 0);
    PbWriter conn;
    const std::string ip = WiFi.localIP().toString().c_str();
    conn.bytes(1, ip.data(), ip.size());
    conn.u(2, 3);
    conn.bytes(3, g_ssid.data(), g_ssid.size());
    const uint8_t* bssid = WiFi.BSSID();
    if (bssid) conn.bytes(4, bssid, 6);
    conn.i32(5, WiFi.channel());
    st.sub(11, conn.out);
    setStage(Stage::WifiConnected);
  } else if (wl == WL_NO_SSID_AVAIL || wl == WL_CONNECT_FAILED || millis() - g_appliedAt > WIFI_CONNECT_TIMEOUT_MS) {
    st.u(2, 3);                                // ConnectionFailed
    st.u(10, wl == WL_NO_SSID_AVAIL ? 1 : 0);  // NotFound / AuthError
    setStage(Stage::WifiFailed);
  } else {
    st.u(2, 1);  // Connecting
  }
  PbWriter resp;
  resp.u(1, 1);  // TypeRespGetWifiStatus
  resp.sub(11, st.out);
  return resp.out;
}

std::string handleConfig(const std::string& req) {
  const auto* data = reinterpret_cast<const uint8_t*>(req.data());
  const uint32_t msg = static_cast<uint32_t>(fieldValue(data, req.size(), 1));
  PbWriter resp;
  PbWriter status;
  status.u(1, 0);
  switch (msg) {
    case 0: {
      std::string out = wifiStatus();
      logf("config: status asked, wl=%d", static_cast<int>(WiFi.status()));
      return out;
    }
    case 2: {
      PbField cmd;
      PbField f;
      if (!findField(data, req.size(), 12, cmd)) return {};
      std::string ssidText;
      std::string pass;
      if (findField(cmd, 1, f)) ssidText.assign(reinterpret_cast<const char*>(f.data), f.len);
      if (findField(cmd, 2, f)) pass.assign(reinterpret_cast<const char*>(f.data), f.len);
      {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ssid = ssidText;
        g_pass = pass;
      }
      logf("config: Wi-Fi details for \"%s\" (password %u chars)", ssidText.c_str(),
           static_cast<unsigned>(pass.size()));
      resp.u(1, 3);
      resp.sub(13, status.out);
      return resp.out;
    }
    case 4: {
      logf("config: apply, joining \"%s\"", g_ssid.c_str());
      WiFi.disconnect(false);
      WiFi.begin(g_ssid.c_str(), g_pass.c_str());
      g_applied = true;
      g_appliedAt = millis();
      setStage(Stage::WifiConnecting);
      resp.u(1, 5);
      resp.sub(15, status.out);
      return resp.out;
    }
    default:
      logf("config: unknown message %u", static_cast<unsigned>(msg));
      return {};
  }
}

// ---- prov-scan ----
int g_scanCount = 0;
bool g_scanDone = false;

std::string handleScan(const std::string& req) {
  const auto* data = reinterpret_cast<const uint8_t*>(req.data());
  const uint32_t msg = static_cast<uint32_t>(fieldValue(data, req.size(), 1));
  PbWriter resp;
  switch (msg) {
    case 0: {
      PbField cmd;
      bool passive = false;
      if (findField(data, req.size(), 10, cmd)) passive = fieldValue(cmd.data, cmd.len, 2) != 0;
      // The app asks for a blocking scan, so the reply waits for the results
      // as on the stock firmware.
      g_scanDone = false;
      const int n = WiFi.scanNetworks(false, false, passive);
      g_scanCount = n < 0 ? 0 : std::min<int>(n, MAX_SCAN_RESULTS);
      g_scanDone = true;
      logf("scan: %d networks", n);
      resp.u(1, 1);
      resp.u(2, 0);
      resp.sub(11, std::string());
      return resp.out;
    }
    case 2: {
      PbWriter st;
      st.u(1, g_scanDone ? 1 : 0);
      st.u(2, static_cast<uint32_t>(g_scanCount));
      resp.u(1, 3);
      resp.u(2, 0);
      resp.sub(13, st.out);
      return resp.out;
    }
    case 4: {
      PbField cmd;
      uint32_t start = 0;
      uint32_t count = 0;
      if (findField(data, req.size(), 14, cmd)) {
        start = static_cast<uint32_t>(fieldValue(cmd.data, cmd.len, 1));
        count = static_cast<uint32_t>(fieldValue(cmd.data, cmd.len, 2));
      }
      PbWriter list;
      for (uint32_t i = start; i < start + count && static_cast<int>(i) < g_scanCount; ++i) {
        PbWriter entry;
        const String name = WiFi.SSID(i);
        entry.bytes(1, name.c_str(), name.length());
        entry.u(2, static_cast<uint32_t>(WiFi.channel(i)));
        entry.i32(3, WiFi.RSSI(i));
        const uint8_t* bssid = WiFi.BSSID(i);
        if (bssid) entry.bytes(4, bssid, 6);
        entry.u(5, authModeFor(WiFi.encryptionType(i)));
        list.sub(1, entry.out);
      }
      resp.u(1, 5);
      resp.u(2, 0);
      resp.sub(15, list.out);
      logf("scan: sent results %u..%u", static_cast<unsigned>(start), static_cast<unsigned>(start + count));
      return resp.out;
    }
    default:
      logf("scan: unknown message %u", static_cast<unsigned>(msg));
      return {};
  }
}

// ---- prov-ctrl ----
std::string handleCtrl(const std::string& req) {
  const auto* data = reinterpret_cast<const uint8_t*>(req.data());
  const uint32_t msg = static_cast<uint32_t>(fieldValue(data, req.size(), 1));
  PbWriter resp;
  if (msg == 1 || msg == 3) {
    logf("ctrl: %s", msg == 1 ? "Wi-Fi reset" : "reprovision");
    WiFi.disconnect(false);
    g_applied = false;
    setStage(Stage::SessionReady);
    resp.u(1, msg + 1);
    resp.u(2, 0);
    resp.sub(msg == 1 ? 12 : 14, std::string());
    return resp.out;
  }
  logf("ctrl: unknown message %u", static_cast<unsigned>(msg));
  return {};
}

// First bytes of a GATT value as hex, for the log.
std::string hexHead(const uint8_t* data, const size_t len) {
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < len && i < 24; ++i) {
    out.push_back(DIGITS[data[i] >> 4]);
    out.push_back(DIGITS[data[i] & 0xF]);
  }
  if (len > 24) out += "..";
  return out;
}

// ---- GATT glue: the app writes a request, then reads the reply ----
class EndpointCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit EndpointCallbacks(const char* name) : name(name) {}

  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    logf("read %s (%u bytes)", name, static_cast<unsigned>(c->getValue().length()));
  }

  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, const uint16_t subValue) override {
    logf("subscribe %s = %u", name, subValue);
  }

  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    const NimBLEAttValue value = c->getValue();
    const std::string raw(reinterpret_cast<const char*>(value.data()), value.length());
    logf("write %s (%u bytes) %s", name, static_cast<unsigned>(raw.size()),
         hexHead(value.data(), value.length()).c_str());
    std::string reply;
    if (strcmp(name, "proto-ver") == 0) {
      reply = VERSION_JSON;
    } else if (strcmp(name, "prov-session") == 0) {
      reply = handleSession(raw);
    } else if (!g_session.ready) {
      logf("%s: no secure session yet", name);
    } else {
      const std::string plain = g_session.crypt(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
      std::string out;
      if (strcmp(name, "prov-config") == 0) {
        out = handleConfig(plain);
      } else if (strcmp(name, "prov-scan") == 0) {
        out = handleScan(plain);
      } else if (strcmp(name, "prov-ctrl") == 0) {
        out = handleCtrl(plain);
      }
      reply = g_session.crypt(reinterpret_cast<const uint8_t*>(out.data()), out.size());
    }
    c->setValue(reinterpret_cast<const uint8_t*>(reply.data()), reply.size());
    logf("reply %s (%u bytes)", name, static_cast<unsigned>(reply.size()));
  }

 private:
  const char* name;
};

class DescriptorCallbacks final : public NimBLEDescriptorCallbacks {
 public:
  void onRead(NimBLEDescriptor* d, NimBLEConnInfo&) override {
    logf("read name of %s", d->getCharacteristic()->getUUID().toString().c_str());
  }
};
DescriptorCallbacks g_descriptorCallbacks;

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    g_session.reset();
    setStage(Stage::Connected);
    // Stock does this in its GAP CONNECT listener (ble_svc_gatt_changed over
    // the whole range) so a phone that cached the stock table rediscovers.
    server->sendServiceChangedIndication();
    logf("app connected, MTU %u, peer %s (%s), handle %u, bonded %d, encrypted %d", info.getMTU(),
         info.getAddress().toString().c_str(), info.getAddress().isPublic() ? "public" : "random", info.getConnHandle(),
         info.isBonded(), info.isEncrypted());
  }
  void onConnParamsUpdate(NimBLEConnInfo& info) override {
    logf("conn params: interval %u, latency %u, timeout %u", info.getConnInterval(), info.getConnLatency(),
         info.getConnTimeout());
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    logf("auth complete: bonded %d, encrypted %d", info.isBonded(), info.isEncrypted());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, const int reason) override {
    g_session.reset();
    logf("app disconnected, reason %d", reason);
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (g_stage == Stage::Connected || g_stage == Stage::SessionReady) g_stage = Stage::Advertising;
  }
  void onMTUChange(const uint16_t mtu, NimBLEConnInfo&) override { logf("MTU now %u", mtu); }
};

ServerCallbacks g_serverCallbacks;
std::vector<EndpointCallbacks*> g_endpointCallbacks;

std::string efuseHex() {
  char buf[17];
  snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(ESP.getEfuseMac()));
  return buf;
}

std::string makePop() {
  static constexpr char ALPHABET[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string pop;
  for (int i = 0; i < 8; ++i) pop.push_back(ALPHABET[esp_random() % (sizeof(ALPHABET) - 1)]);
  return pop;
}

}  // namespace

bool start(std::string& qrUrl) {
  stop();
  const std::string name = "Picco-" + efuseHex();
  g_pop = makePop();
  g_applied = false;
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_ssid.clear();
    g_pass.clear();
  }

  // The page turner owns NimBLE as a central; release it first.
  bleturner::stopNow(3000);
  // Also frees objects a previous stop() left behind (safe with the host off).
  NimBLEDevice::deinit(true);
  for (auto* cb : g_endpointCallbacks) delete cb;
  g_endpointCallbacks.clear();
  WiFi.mode(WIFI_STA);

  if (!NimBLEDevice::init(name)) {
    logf("NimBLE init failed");
    setStage(Stage::Failed);
    return false;
  }
  NimBLEDevice::setMTU(256);
  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(&g_serverCallbacks, false);
  g_server->advertiseOnDisconnect(true);
  NimBLEService* service = g_server->createService(SERVICE_UUID);
  for (const auto& ep : ENDPOINTS) {
    NimBLECharacteristic* c =
        service->createCharacteristic(ep.uuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE, 512);
    NimBLEDescriptor* desc = c->createDescriptor("2901", NIMBLE_PROPERTY::READ, strlen(ep.name));
    desc->setValue(ep.name);
    desc->setCallbacks(&g_descriptorCallbacks);
    auto* cb = new (std::nothrow) EndpointCallbacks(ep.name);
    if (cb) {
      g_endpointCallbacks.push_back(cb);
      c->setCallbacks(cb);
    }
  }
  service->start();

  // Service UUID in the advertisement, name in the scan response (both do not
  // fit in 31 bytes), as Espressif's scheme_ble does.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setCompleteServices(NimBLEUUID(SERVICE_UUID));
  NimBLEAdvertisementData scanData;
  scanData.setName(name);
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);
  if (!adv->start()) {
    logf("advertising failed");
    setStage(Stage::Failed);
    return false;
  }
  setStage(Stage::Advertising);

  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macParam[40];
  snprintf(macParam, sizeof(macParam), "%02X%%3A%02X%%3A%02X%%3A%02X%%3A%02X%%3A%02X", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  qrUrl = "https://www.booxtiles.com/download/?ver=v1.0.5&model=picco&name=" + name + "&pop=" + g_pop +
          "&transport=ble&hostname=" + name + ".local&macAddress=" + macParam;
  logf("advertising as %s, heap %u, PSRAM free %u", name.c_str(), static_cast<unsigned>(ESP.getFreeHeap()),
       static_cast<unsigned>(ESP.getFreePsram()));
  return true;
}

void stop() {
  if (NimBLEDevice::isInitialized() && g_server) {
    // Each step is written to the log before it runs, so a crash shows where.
    auto step = [](const char* what) {
      flushLog();
      tilescloud::logLine("[bind] stop: %s", what);
    };
    step("advertising off");
    g_server->advertiseOnDisconnect(false);
    NimBLEDevice::getAdvertising()->stop();
    step("disconnect");
    for (const uint16_t handle : g_server->getPeerDevices()) g_server->disconnect(handle);
    for (int i = 0; i < 25 && g_server->getConnectedCount() > 0; ++i) delay(20);
    g_server->setCallbacks(nullptr, false);
    step("host stop");
    // Keep the GATT objects until the next start(): a host event still in
    // flight may point at them.
    const bool stopped = NimBLEDevice::deinit(false);
    tilescloud::logLine("[bind] stop: host stopped %d", stopped);
  }
  g_server = nullptr;
  g_session.reset();
  setStage(Stage::Off);
}

Stage stage() {
  std::lock_guard<std::mutex> lock(g_stateMutex);
  return g_stage;
}

std::string ssid() {
  std::lock_guard<std::mutex> lock(g_stateMutex);
  return g_ssid;
}

std::string passphrase() {
  std::lock_guard<std::mutex> lock(g_stateMutex);
  return g_pass;
}

void flushLog() {
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    lines.swap(g_logQueue);
  }
  for (const auto& line : lines) tilescloud::logLine("[bind] %s", line.c_str());
}

}  // namespace tilesbind

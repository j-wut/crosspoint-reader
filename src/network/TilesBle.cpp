#include "TilesBle.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#ifdef USING_NIMBLE_ARDUINO_HEADERS
#include "nimble/nimble/host/include/host/ble_hs.h"
#include "nimble/nimble/host/services/gatt/include/services/gatt/ble_svc_gatt.h"
#include "nimble/nimble/host/util/include/host/util/util.h"
#else
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

#include <cstdarg>
#include <cstring>
#include <mutex>
#include <vector>

namespace tilesble {
namespace {

// As PROTOCOMM_NIMBLE_MAX_PAYLOAD_LEN in protocomm_nimble.c.
constexpr uint16_t MAX_PAYLOAD = 512;
constexpr size_t MAX_ENDPOINTS = 8;

Callbacks g_cb{};
std::string g_name;
size_t g_count = 0;
const Endpoint* g_endpoints = nullptr;

// The GATT table, built as populate_gatt_db() does: one primary service,
// each endpoint a READ|WRITE characteristic with a 0x2901 user description.
ble_uuid128_t g_serviceUuid;
ble_uuid128_t g_chrUuid[MAX_ENDPOINTS];
const ble_uuid16_t DESCRIPTION_UUID = BLE_UUID16_INIT(0x2901);
ble_gatt_dsc_def g_dsc[MAX_ENDPOINTS][2];
ble_gatt_chr_def g_chr[MAX_ENDPOINTS + 1];
ble_gatt_svc_def g_svc[2];
uint16_t g_valueHandle[MAX_ENDPOINTS];

// Last reply per endpoint; a read returns it (simple_ble_gatts_get_attr_value).
std::mutex g_replyMutex;
std::string g_reply[MAX_ENDPOINTS];

uint8_t g_ownAddrType = BLE_OWN_ADDR_PUBLIC;
volatile uint16_t g_connHandle = BLE_HS_CONN_HANDLE_NONE;
volatile bool g_running = false;
bool g_started = false;  // the host is ours to stop

void logf(const char* format, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* format, ...) {
  if (!g_cb.log) return;
  char line[200];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  g_cb.log(line);
}

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

void clearReplies() {
  std::lock_guard<std::mutex> lock(g_replyMutex);
  for (auto& reply : g_reply) reply.clear();
}

int chrAccess(uint16_t, uint16_t attrHandle, ble_gatt_access_ctxt* ctxt, void* arg) {
  const size_t index = reinterpret_cast<uintptr_t>(arg);
  if (index >= g_count) return BLE_ATT_ERR_UNLIKELY;
  const char* name = g_endpoints[index].name;
  switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
      std::string reply;
      {
        std::lock_guard<std::mutex> lock(g_replyMutex);
        reply = g_reply[index];
      }
      logf("read %s, handle %u (%u bytes)", name, attrHandle, static_cast<unsigned>(reply.size()));
      if (reply.empty()) return 0;
      return os_mbuf_append(ctxt->om, reply.data(), reply.size()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
      const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
      if (len > MAX_PAYLOAD) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      std::string request(len, '\0');
      uint16_t copied = 0;
      if (ble_hs_mbuf_to_flat(ctxt->om, request.data(), len, &copied) != 0) return BLE_ATT_ERR_UNLIKELY;
      request.resize(copied);
      logf("write %s, handle %u (%u bytes) %s", name, attrHandle, static_cast<unsigned>(request.size()),
           hexHead(reinterpret_cast<const uint8_t*>(request.data()), request.size()).c_str());
      std::string reply = g_cb.onWrite ? g_cb.onWrite(index, request) : std::string();
      logf("reply %s (%u bytes)", name, static_cast<unsigned>(reply.size()));
      std::lock_guard<std::mutex> lock(g_replyMutex);
      g_reply[index] = std::move(reply);
      return 0;
    }
    default:
      return BLE_ATT_ERR_UNLIKELY;
  }
}

int dscAccess(uint16_t, uint16_t attrHandle, ble_gatt_access_ctxt* ctxt, void* arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) return BLE_ATT_ERR_UNLIKELY;
  const char* name = static_cast<const char*>(arg);
  logf("read name %s, handle %u", name, attrHandle);
  return os_mbuf_append(ctxt->om, name, strlen(name)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

void buildTable(const uint8_t serviceUuid[16]) {
  memset(g_dsc, 0, sizeof(g_dsc));
  memset(g_chr, 0, sizeof(g_chr));
  memset(g_svc, 0, sizeof(g_svc));
  g_serviceUuid.u.type = BLE_UUID_TYPE_128;
  memcpy(g_serviceUuid.value, serviceUuid, 16);
  for (size_t i = 0; i < g_count; ++i) {
    g_chrUuid[i] = g_serviceUuid;
    g_chrUuid[i].value[12] = g_endpoints[i].uuid16 & 0xFF;
    g_chrUuid[i].value[13] = g_endpoints[i].uuid16 >> 8;
    g_dsc[i][0].uuid = &DESCRIPTION_UUID.u;
    g_dsc[i][0].att_flags = BLE_ATT_F_READ;
    g_dsc[i][0].access_cb = dscAccess;
    g_dsc[i][0].arg = const_cast<char*>(g_endpoints[i].name);
    g_chr[i].uuid = &g_chrUuid[i].u;
    g_chr[i].access_cb = chrAccess;
    g_chr[i].arg = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
    g_chr[i].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE;
    g_chr[i].descriptors = g_dsc[i];
    g_chr[i].val_handle = &g_valueHandle[i];
  }
  g_svc[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  g_svc[0].uuid = &g_serviceUuid.u;
  g_svc[0].characteristics = g_chr;
}

void advertise();

// simple_ble_gap_event() plus logging.
int gapEvent(ble_gap_event* event, void*) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
      if (event->connect.status != 0) {
        logf("connect failed, status %d", event->connect.status);
        advertise();
        return 0;
      }
      g_connHandle = event->connect.conn_handle;
      clearReplies();
      ble_gap_conn_desc desc{};
      if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
        logf("app connected, handle %u, peer %02x:%02x:%02x:%02x:%02x:%02x (type %u), encrypted %d, bonded %d",
             event->connect.conn_handle, desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], desc.peer_id_addr.val[3],
             desc.peer_id_addr.val[2], desc.peer_id_addr.val[1], desc.peer_id_addr.val[0], desc.peer_id_addr.type,
             desc.sec_state.encrypted, desc.sec_state.bonded);
      }
      // The stock GAP listener indicates Service Changed over the whole range
      // when the firmware changed, so a phone with a cached table rediscovers.
      ble_svc_gatt_changed(0x0001, 0xffff);
      if (g_cb.onConnect) g_cb.onConnect();
      return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT:
      logf("app disconnected, reason %d", event->disconnect.reason);
      g_connHandle = BLE_HS_CONN_HANDLE_NONE;
      clearReplies();
      if (g_cb.onDisconnect) g_cb.onDisconnect(event->disconnect.reason);
      advertise();
      return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertise();
      return 0;
    case BLE_GAP_EVENT_MTU:
      logf("MTU now %u", event->mtu.value);
      return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
      logf("%s tx, handle %u, status %d", event->notify_tx.indication ? "indication" : "notify",
           event->notify_tx.attr_handle, event->notify_tx.status);
      return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
      logf("subscribe handle %u: notify %d->%d, indicate %d->%d, reason %d", event->subscribe.attr_handle,
           event->subscribe.prev_notify, event->subscribe.cur_notify, event->subscribe.prev_indicate,
           event->subscribe.cur_indicate, event->subscribe.reason);
      return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
      logf("encryption change, status %d", event->enc_change.status);
      return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
      logf("connection update, status %d", event->conn_update.status);
      return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
      logf("repeat pairing");
      return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      logf("passkey action %d", event->passkey.params.action);
      return 0;
    default:
      return 0;
  }
}

// simple_ble_advertise(): flags + 128-bit service UUID, name in the scan
// response, undirected connectable, interval 0x100.
void advertise() {
  if (!g_running) return;
  ble_hs_adv_fields adv{};
  adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  adv.uuids128 = &g_serviceUuid;
  adv.num_uuids128 = 1;
  adv.uuids128_is_complete = 1;
  int rc = ble_gap_adv_set_fields(&adv);
  if (rc != 0) {
    logf("set advertisement data failed, rc %d", rc);
    return;
  }
  ble_hs_adv_fields rsp{};
  rsp.name = reinterpret_cast<const uint8_t*>(g_name.c_str());
  rsp.name_len = static_cast<uint8_t>(g_name.size());
  rsp.name_is_complete = 1;
  rc = ble_gap_adv_rsp_set_fields(&rsp);
  if (rc != 0) {
    logf("set scan response failed, rc %d", rc);
    return;
  }
  ble_gap_adv_params params{};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = 0x100;
  params.itvl_max = 0x100;
  rc = ble_gap_adv_start(g_ownAddrType, nullptr, BLE_HS_FOREVER, &params, gapEvent, nullptr);
  if (rc != 0 && rc != BLE_HS_EALREADY) logf("advertising failed, rc %d", rc);
}

}  // namespace

bool start(const std::string& deviceName, const uint8_t serviceUuid[16], const Endpoint* endpoints, const size_t count,
           const Callbacks& callbacks) {
  if (count == 0 || count > MAX_ENDPOINTS) return false;
  g_cb = callbacks;
  g_name = deviceName;
  g_endpoints = endpoints;
  g_count = count;
  clearReplies();

  // Brings up the controller and NimBLE host only; no NimBLE-Arduino server.
  if (!NimBLEDevice::init(deviceName)) {
    logf("NimBLE init failed");
    return false;
  }
  g_started = true;
  NimBLEDevice::setMTU(256);

  // Security as simple_ble_start(): Just Works, no bonding, MITM and Secure
  // Connections flags set (scheme_ble sets ble_sm_sc), LTK and IRK.
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  ble_hs_cfg.sm_bonding = 0;
  ble_hs_cfg.sm_mitm = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  // gatt_svr_init(): the GATT service and our service only. protocomm never
  // calls ble_svc_gap_init(), so there is no GAP service in the table.
  buildTable(serviceUuid);
  ble_gatts_reset();
  ble_svc_gatt_init();
  int rc = ble_gatts_count_cfg(g_svc);
  if (rc == 0) rc = ble_gatts_add_svcs(g_svc);
  if (rc == 0) rc = ble_gatts_start();
  if (rc != 0) {
    logf("GATT setup failed, rc %d", rc);
    return false;
  }
  uint16_t serviceHandle = 0;
  ble_gatts_find_svc(&g_serviceUuid.u, &serviceHandle);
  logf("service handle %u", serviceHandle);
  for (size_t i = 0; i < count; ++i) {
    uint16_t nameHandle = 0;
    ble_gatts_find_dsc(&g_serviceUuid.u, &g_chrUuid[i].u, &DESCRIPTION_UUID.u, &nameHandle);
    logf("%s: value handle %u, name handle %u", endpoints[i].name, g_valueHandle[i], nameHandle);
  }

  // simple_ble_on_sync(): public address, no privacy.
  rc = ble_hs_util_ensure_addr(0);
  if (rc == 0) rc = ble_hs_id_infer_auto(0, &g_ownAddrType);
  if (rc != 0) {
    logf("address setup failed, rc %d", rc);
    return false;
  }
  g_running = true;
  advertise();
  return true;
}

void stop() {
  if (!g_started) return;
  g_started = false;
  g_running = false;
  logf("stop: advertising off");
  ble_gap_adv_stop();
  if (g_connHandle != BLE_HS_CONN_HANDLE_NONE) {
    logf("stop: disconnect");
    ble_gap_terminate(g_connHandle, BLE_ERR_REM_USER_CONN_TERM);
    for (int i = 0; i < 25 && g_connHandle != BLE_HS_CONN_HANDLE_NONE; ++i) delay(20);
  }
  // Leave the host as the page turner expects it: no GATT table of ours.
  logf("stop: GATT reset %d", ble_gatts_reset());
  logf("stop: host stop");
  const bool stopped = NimBLEDevice::deinit(false);
  logf("stop: host stopped %d", stopped);
  g_connHandle = BLE_HS_CONN_HANDLE_NONE;
  g_cb = {};
}

}  // namespace tilesble

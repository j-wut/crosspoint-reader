#include "BooxLan.h"

#include <ESPmDNS.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "BooxDiag.h"
#include "BooxSd.h"
#include "network/TilesCloud.h"

namespace booxlan {
namespace {

// Values below are the stock firmware's.
constexpr size_t SLOT_SIZE = 32768;
constexpr int SLOT_COUNT = 3;
constexpr uint32_t IDLE_TIMEOUT_MS = 30000;
constexpr uint64_t SPACE_MARGIN = 65536;
constexpr TickType_t QUEUE_SEND_WAIT = 50;
constexpr TickType_t UPLOAD_LOCK_WAIT = 200;
// Diagnostics only.
constexpr uint32_t SLOW_WRITE_MS = 300;
constexpr uint32_t PROGRESS_STEP = 256 * 1024;

// The stock firmware's page for GET /, byte for byte.
constexpr const char INDEX_HTML[] = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>E-Ink Upload</title>
<style>
body{font-family:sans-serif;max-width:600px;margin:40px auto;padding:0 20px}
.box{border:2px dashed #ccc;border-radius:8px;padding:30px;text-align:center}
input[type=file]{margin:20px 0}
input[type=submit]{background:#4CAF50;color:#fff;border:none;padding:10px 30px;border-radius:4px;cursor:pointer;font-size:16px}
input[type=submit]:hover{background:#45a049}
.progress{width:100%;background:#eee;border-radius:4px;margin:10px 0}
.bar{height:20px;background:#4CAF50;border-radius:4px;width:0%;transition:width .2s}
#status{margin-top:20px;word-break:break-all}
</style>
</head>
<body>
<h2>E-Ink File Upload</h2>
<div class="box">
<form id="f">
<label for="ft">Type:</label>
<select id="ft"><option value="0">Books</option><option value="1">Images</option><option value="2">Fonts</option></select>
<input type="file" id="fi" name="file"><br>
<div class="progress"><div class="bar" id="pb"></div></div>
<input type="submit" value="Upload">
</form>
<div id="status"></div>
</div>
<script>
document.getElementById('f').addEventListener('submit',function(e){
 e.preventDefault();
 var f=document.getElementById('fi').files[0];
 if(!f){alert('Please select a file');return;}
 var fd=new FormData();fd.append('file',f);
 var x=new XMLHttpRequest();
 x.open('POST','/upload?fileType='+document.getElementById('ft').value,true);
 x.upload.onprogress=function(e){
  if(e.lengthComputable){
   var p=(e.loaded/e.total*100);
   document.getElementById('pb').style.width=p+'%';
   document.getElementById('status').textContent=p.toFixed(1)+'%';
  }
 };
 x.onload=function(){document.getElementById('status').textContent=x.responseText;};
 x.onerror=function(){document.getElementById('status').textContent='Upload failed!';};
 x.send(fd);
});
</script>
</body>
</html>
)HTML";

// One buffer handed to the writer task.
struct WriteItem {
  uint8_t* data;
  uint32_t len;
  uint32_t slot;
};

// Shared with the writer task.
uint8_t* g_slots[SLOT_COUNT] = {};
QueueHandle_t g_freeSlots = nullptr;  // indexes of free slots
QueueHandle_t g_writeQueue = nullptr;
SemaphoreHandle_t g_writeDone = nullptr;
SemaphoreHandle_t g_uploadLock = nullptr;
TaskHandle_t g_writerTask = nullptr;
volatile int g_pending = 0;
volatile bool g_writeFailed = false;
booxsd::File g_file;
std::string g_path;
// Writer statistics, for the log.
volatile uint32_t g_writeCount = 0;
volatile uint32_t g_writeMsTotal = 0;
volatile uint32_t g_writeMsMax = 0;
volatile uint32_t g_slowWrites = 0;

httpd_handle_t g_server = nullptr;
bool g_mdns = false;

portMUX_TYPE g_statusLock = portMUX_INITIALIZER_UNLOCKED;
Status g_status;
volatile uint32_t g_statusVersion = 0;

template <typename F>
void updateStatus(F&& change) {
  portENTER_CRITICAL(&g_statusLock);
  change(g_status);
  portEXIT_CRITICAL(&g_statusLock);
  g_statusVersion = g_statusVersion + 1;
}

void releaseSlot(const int slot) { xQueueSend(g_freeSlots, &slot, 0); }

// The "httpSd" task: writes each buffer to the open file, in order.
void writerTask(void*) {
  WriteItem item;
  for (;;) {
    if (xQueueReceive(g_writeQueue, &item, portMAX_DELAY) != pdTRUE) continue;
    if (!g_writeFailed && g_file) {
      const unsigned long start = millis();
      const size_t written = g_file.write(item.data, item.len);
      const uint32_t ms = millis() - start;
      g_writeCount = g_writeCount + 1;
      g_writeMsTotal = g_writeMsTotal + ms;
      if (ms > g_writeMsMax) g_writeMsMax = ms;
      if (ms > SLOW_WRITE_MS) {
        g_slowWrites = g_slowWrites + 1;
        LOG_INF("BOOXLAN", "slow write: %u bytes in %u ms", static_cast<unsigned>(item.len), static_cast<unsigned>(ms));
      }
      if (written != item.len) {
        LOG_ERR("BOOXLAN", "SD write short %u/%u path=%s", static_cast<unsigned>(written),
                static_cast<unsigned>(item.len), g_path.c_str());
        g_writeFailed = true;
      }
    }
    releaseSlot(static_cast<int>(item.slot));
    portENTER_CRITICAL(&g_statusLock);
    g_pending = g_pending - 1;
    portEXIT_CRITICAL(&g_statusLock);
    xSemaphoreGive(g_writeDone);
  }
}

bool initWriter() {
  if (g_writerTask) return true;
  for (auto& slot : g_slots) {
    slot = static_cast<uint8_t*>(heap_caps_malloc(SLOT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!slot) {
      LOG_ERR("BOOXLAN", "BufferPool init failed");
      return false;
    }
  }
  g_freeSlots = xQueueCreate(SLOT_COUNT, sizeof(int));
  for (int i = 0; g_freeSlots && i < SLOT_COUNT; i++) releaseSlot(i);
  g_writeQueue = xQueueCreate(SLOT_COUNT, sizeof(WriteItem));
  g_writeDone = xSemaphoreCreateBinary();
  if (!g_freeSlots || !g_writeQueue || !g_writeDone) {
    LOG_ERR("BOOXLAN", "Failed to create write queue/sem");
    return false;
  }
  g_uploadLock = xSemaphoreCreateMutex();
  if (!g_uploadLock) {
    LOG_ERR("BOOXLAN", "Failed to create upload mutex");
    return false;
  }
  if (xTaskCreate(writerTask, "httpSd", 4096, nullptr, 2, &g_writerTask) != pdPASS) {
    LOG_ERR("BOOXLAN", "Failed to create SD write task");
    g_writerTask = nullptr;
    return false;
  }
  return true;
}

// Take a free buffer for receiving. Returns its index, or -1.
int acquireSlot(uint32_t& waitCount, uint32_t& waitMs) {
  int slot = -1;
  if (xQueueReceive(g_freeSlots, &slot, 0) == pdTRUE) return slot;
  waitCount++;
  const unsigned long start = millis();
  const bool ok = xQueueReceive(g_freeSlots, &slot, pdMS_TO_TICKS(IDLE_TIMEOUT_MS)) == pdTRUE;
  waitMs += millis() - start;
  return ok ? slot : -1;
}

bool submit(const int slot, const uint8_t* data, const uint32_t len) {
  if (len == 0) {
    releaseSlot(slot);
    return true;
  }
  WriteItem item{const_cast<uint8_t*>(data), len, static_cast<uint32_t>(slot)};
  portENTER_CRITICAL(&g_statusLock);
  g_pending = g_pending + 1;
  portEXIT_CRITICAL(&g_statusLock);
  while (xQueueSend(g_writeQueue, &item, QUEUE_SEND_WAIT) != pdTRUE) {
  }
  return true;
}

void drainWriter() {
  while (g_pending > 0) xSemaphoreTake(g_writeDone, pdMS_TO_TICKS(1000));
}

void setCors(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "HEAD, GET, POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
}

const char* uploadDir(httpd_req_t* req) {
  char query[64];
  char value[8];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return "/books";
  if (httpd_query_key_value(query, "fileType", value, sizeof(value)) != ESP_OK) return "/books";
  switch (atoi(value)) {
    case 0:
      return "/books";
    case 1:
      return "/images";
    case 2:
      return "/fonts";
    default:
      return "/.onyx";
  }
}

const uint8_t* findBytes(const uint8_t* hay, const size_t hayLen, const char* needle, const size_t needleLen) {
  if (needleLen == 0 || hayLen < needleLen) return nullptr;
  const uint8_t* end = hay + hayLen - needleLen + 1;
  for (const uint8_t* p = hay; p < end; ++p) {
    p = static_cast<const uint8_t*>(memchr(p, needle[0], end - p));
    if (!p) return nullptr;
    if (memcmp(p, needle, needleLen) == 0) return p;
  }
  return nullptr;
}

struct Upload {
  const char* error = nullptr;
  std::string name;
  uint32_t fileBytes = 0;
  uint32_t received = 0;
  // Diagnostics.
  uint32_t recvCalls = 0;
  uint32_t recvTimeouts = 0;
  uint32_t recvMsMax = 0;
  uint32_t slotWaits = 0;
  uint32_t slotWaitMs = 0;
  uint32_t nextProgress = PROGRESS_STEP;
};

// Open the destination for the part whose headers are `headers`.
const char* openDestination(Upload& up, const char* dir, const std::string& headers) {
  const size_t at = headers.find("filename=\"");
  if (at != std::string::npos) {
    const size_t start = at + strlen("filename=\"");
    const size_t end = headers.find('"', start);
    up.name = headers.substr(start, end == std::string::npos ? std::string::npos : end - start);
  }
  if (up.name.empty()) up.name = "upload.bin";
  if (up.name == "." || up.name == ".." || up.name.find("..") != std::string::npos) return "Failed to create file";
  if (!booxsd::exists(dir) && !booxsd::mkdir(dir)) return "Failed to create directory";
  char path[320];
  snprintf(path, sizeof(path), "%s/%s", dir, up.name.c_str());
  g_path = path;
  g_writeFailed = false;
  g_file = booxsd::openWrite(path);
  if (!g_file) return "Failed to create file";
  LOG_INF("BOOXLAN", "Upload start: %s %s", up.name.c_str(), path);
  return nullptr;
}

void logUploadStats(const Upload& up, const size_t total, const unsigned long elapsedMs) {
  const uint32_t writes = g_writeCount;
  LOG_INF("BOOXLAN",
          "stats: %u of %u bytes in %lu ms (%lu KB/s), file %u bytes, recv calls %u timeouts %u max %u ms, "
          "buffer waits %u (%u ms), writes %u avg %u ms max %u ms slow %u",
          static_cast<unsigned>(up.received), static_cast<unsigned>(total), elapsedMs,
          elapsedMs ? static_cast<unsigned long>(up.received / elapsedMs * 1000 / 1024) : 0UL,
          static_cast<unsigned>(up.fileBytes), static_cast<unsigned>(up.recvCalls),
          static_cast<unsigned>(up.recvTimeouts), static_cast<unsigned>(up.recvMsMax),
          static_cast<unsigned>(up.slotWaits), static_cast<unsigned>(up.slotWaitMs), static_cast<unsigned>(writes),
          writes ? static_cast<unsigned>(g_writeMsTotal / writes) : 0U, static_cast<unsigned>(g_writeMsMax),
          static_cast<unsigned>(g_slowWrites));
}

// Receive the multipart body. Returns nullptr on success, else the error text.
const char* receive(httpd_req_t* req, Upload& up, const size_t total, const std::string& marker, const char* dir) {
  int slot = acquireSlot(up.slotWaits, up.slotWaitMs);
  if (slot < 0) return "No upload buffer";
  uint8_t* buf = g_slots[slot];
  size_t used = 0;            // bytes in buf
  bool inHeaders = true;      // still reading the part headers
  size_t dataStart = 0;       // file data start in buf
  unsigned long lastData = millis();

  for (;;) {
    if (up.received >= total) {
      LOG_INF("BOOXLAN", "Upload EOF after full Content-Length, treat as done file=%u",
              static_cast<unsigned>(up.fileBytes));
      if (!inHeaders) {
        up.fileBytes += used - dataStart;
        submit(slot, buf + dataStart, used - dataStart);
      } else {
        releaseSlot(slot);
      }
      return nullptr;
    }
    if (used >= SLOT_SIZE) {
      releaseSlot(slot);
      return inHeaders ? "Headers too large" : "Recv buffer full";
    }
    const size_t want = std::min<size_t>(SLOT_SIZE - used, total - up.received);
    const unsigned long recvStart = millis();
    const int got = httpd_req_recv(req, reinterpret_cast<char*>(buf + used), want);
    const uint32_t recvMs = millis() - recvStart;
    up.recvCalls++;
    if (recvMs > up.recvMsMax) up.recvMsMax = recvMs;
    if (got == HTTPD_SOCK_ERR_TIMEOUT) {
      up.recvTimeouts++;
      if (millis() - lastData > IDLE_TIMEOUT_MS) {
        LOG_ERR("BOOXLAN", "Upload idle timeout after %u ms", static_cast<unsigned>(IDLE_TIMEOUT_MS));
        releaseSlot(slot);
        return "Recv timeout";
      }
      continue;
    }
    if (got <= 0) {
      LOG_ERR("BOOXLAN", "Upload truncated recv=%u/%u file=%u carry=%u phase=%u (recv %d)",
              static_cast<unsigned>(up.received), static_cast<unsigned>(total), static_cast<unsigned>(up.fileBytes),
              static_cast<unsigned>(used - dataStart), inHeaders ? 0U : 1U, got);
      releaseSlot(slot);
      return "Upload truncated";
    }
    lastData = millis();
    up.received += got;
    used += got;
    if (up.received >= up.nextProgress) {
      LOG_INF("BOOXLAN", "progress %u/%u, file %u, pending writes %d", static_cast<unsigned>(up.received),
              static_cast<unsigned>(total), static_cast<unsigned>(up.fileBytes), g_pending);
      while (up.nextProgress <= up.received) up.nextProgress += PROGRESS_STEP;
      updateStatus([&](Status& s) { s.uploadBytes = up.received; });
    }

    if (inHeaders) {
      const uint8_t* end = findBytes(buf, used, "\r\n\r\n", 4);
      if (!end) continue;
      const std::string headers(reinterpret_cast<const char*>(buf), end - buf);
      if (const char* err = openDestination(up, dir, headers)) {
        releaseSlot(slot);
        return err;
      }
      updateStatus([&](Status& s) { snprintf(s.last, sizeof(s.last), "%s", up.name.c_str()); });
      inHeaders = false;
      dataStart = end + 4 - buf;
    }

    // The data ends at "\r\n--boundary".
    const uint8_t* close = findBytes(buf + dataStart, used - dataStart, marker.data(), marker.size());
    if (close) {
      const uint32_t len = close - (buf + dataStart);
      up.fileBytes += len;
      submit(slot, buf + dataStart, len);
      const uint8_t* after = close + marker.size();
      if (after + 2 <= buf + used && memcmp(after, "--", 2) != 0) return "Only single file uploads supported";
      // Read the rest of the body so the reply reaches the client.
      char sink[256];
      while (up.received < total) {
        const int more = httpd_req_recv(req, sink, std::min<size_t>(sizeof(sink), total - up.received));
        if (more == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (more <= 0) break;
        up.received += more;
      }
      return nullptr;
    }
    // No end yet: as in stock, each received chunk is one write, except a
    // possible start of the marker, which moves to the next buffer.
    const size_t avail = used - dataStart;
    const size_t carry = std::min(avail, marker.size() - 1);
    const uint32_t len = avail - carry;
    up.fileBytes += len;
    const int nextSlot = acquireSlot(up.slotWaits, up.slotWaitMs);
    if (nextSlot < 0) {
      submit(slot, buf + dataStart, len);
      return "No upload buffer";
    }
    uint8_t* nextBuf = g_slots[nextSlot];
    if (carry > SLOT_SIZE) {
      submit(slot, buf + dataStart, len);
      releaseSlot(nextSlot);
      return "Boundary carry overflow";
    }
    memcpy(nextBuf, buf + used - carry, carry);
    submit(slot, buf + dataStart, len);
    slot = nextSlot;
    buf = nextBuf;
    used = carry;
    dataStart = 0;
  }
}

esp_err_t handleUpload(httpd_req_t* req) {
  setCors(req);
  if (xSemaphoreTake(g_uploadLock, UPLOAD_LOCK_WAIT) != pdTRUE) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "Busy", HTTPD_RESP_USE_STRLEN);
  }
  booxdiag::logSystemState("upload start");
  const unsigned long start = millis();
  Upload up;
  const size_t total = req->content_len;
  const char* dir = uploadDir(req);
  std::string marker;
  updateStatus([&](Status& s) {
    s.uploading = true;
    s.uploadBytes = 0;
    s.uploadTotal = total;
  });
  g_writeCount = 0;
  g_writeMsTotal = 0;
  g_writeMsMax = 0;
  g_slowWrites = 0;

  char contentType[160];
  if (httpd_req_get_hdr_value_str(req, "Content-Type", contentType, sizeof(contentType)) != ESP_OK) {
    up.error = "Missing Content-Type";
  } else if (const char* b = strstr(contentType, "boundary="); !b) {
    up.error = "Missing boundary";
  } else {
    std::string boundary(b + strlen("boundary="));
    if (!boundary.empty() && boundary.front() == '"') {
      boundary.erase(0, 1);
      const size_t q = boundary.find('"');
      if (q != std::string::npos) boundary.resize(q);
    }
    if (boundary.empty()) {
      up.error = "Empty boundary";
    } else {
      marker = "\r\n--" + boundary;
    }
  }
  LOG_INF("BOOXLAN", "POST /upload dir %s total=%u, content type %s", dir, static_cast<unsigned>(total),
          up.error ? "-" : contentType);
  if (!up.error && !booxsd::mounted()) up.error = "Storage not ready";
  if (!up.error) {
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    const uint64_t needed = static_cast<uint64_t>(total) + SPACE_MARGIN;
    if (booxsd::spaceInfo(totalBytes, freeBytes) && freeBytes < needed) {
      LOG_ERR("BOOXLAN", "SD space insufficient free=%llu needed=%llu", static_cast<unsigned long long>(freeBytes),
              static_cast<unsigned long long>(needed));
      up.error = "Insufficient SD space";
    }
  }
  if (!up.error) up.error = receive(req, up, total, marker, dir);

  drainWriter();
  if (g_file) {
    const unsigned long closeStart = millis();
    g_file.close();
    LOG_INF("BOOXLAN", "close took %lu ms", millis() - closeStart);
  }
  if (!up.error && g_writeFailed) up.error = "SD write failed";
  logUploadStats(up, total, millis() - start);

  esp_err_t result;
  if (!up.error) {
    LOG_INF("BOOXLAN", "Upload done: %s (%u bytes) path=%s", up.name.c_str(), static_cast<unsigned>(up.fileBytes),
            g_path.c_str());
    updateStatus([&](Status& s) {
      s.received++;
      s.uploading = false;
      snprintf(s.last, sizeof(s.last), "%s (%u KB)", up.name.c_str(), static_cast<unsigned>(up.fileBytes / 1024));
    });
    char reply[160];
    snprintf(reply, sizeof(reply), "Upload successful: %s (%u bytes)", up.name.c_str(),
             static_cast<unsigned>(up.fileBytes));
    httpd_resp_set_type(req, "text/plain");
    result = httpd_resp_send(req, reply, HTTPD_RESP_USE_STRLEN);
  } else {
    LOG_ERR("BOOXLAN", "Upload failed: %s (%s)", up.error, g_path.empty() ? "-" : g_path.c_str());
    if (!g_path.empty() && booxsd::mounted()) {
      if (booxsd::remove(g_path.c_str())) {
        LOG_INF("BOOXLAN", "deleted upload file %s", g_path.c_str());
      } else {
        LOG_ERR("BOOXLAN", "delete upload file failed %s", g_path.c_str());
      }
    }
    updateStatus([&](Status& s) {
      s.failed++;
      s.uploading = false;
      snprintf(s.last, sizeof(s.last), "%s", up.error);
    });
    httpd_resp_set_status(req, "500 Internal Server Error");
    result = httpd_resp_send(req, up.error, HTTPD_RESP_USE_STRLEN);
  }
  g_path.clear();
  booxdiag::logSystemState("upload end");
  booxsd::logState("upload end");
  // Keep the whole story on the card.
  if (booxdiag::LogFile log = booxdiag::openLog()) {
    log.printf("\n[%lu s] BOOX LAN upload: %s\n", millis() / 1000, up.error ? up.error : "ok");
    booxdiag::Capture::dumpAndReset(log);
    log.close();
  }
  xSemaphoreGive(g_uploadLock);
  return result;
}

esp_err_t handleOptions(httpd_req_t* req) {
  setCors(req);
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, nullptr, 0);
}

esp_err_t handlePing(httpd_req_t* req) {
  setCors(req);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "pong", HTTPD_RESP_USE_STRLEN);
}

esp_err_t handleIndex(httpd_req_t* req) {
  setCors(req);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
}

}  // namespace

bool start() {
  if (g_server) return true;
  if (!initWriter()) return false;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 0x3000;
  config.core_id = 0;
  config.server_port = 80;
  config.recv_wait_timeout = 1;
  config.max_uri_handlers = 8;
  config.max_open_sockets = 7;
  config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  if (httpd_start(&g_server, &config) != ESP_OK) {
    LOG_ERR("BOOXLAN", "httpd_start failed");
    g_server = nullptr;
    return false;
  }
  const httpd_uri_t routes[] = {
      {"/upload", HTTP_POST, handleUpload, nullptr},
      {"/upload", HTTP_OPTIONS, handleOptions, nullptr},
      {"/ping", HTTP_OPTIONS, handleOptions, nullptr},
      {"/ping", HTTP_HEAD, handlePing, nullptr},
      {"/", HTTP_GET, handleIndex, nullptr},
  };
  for (const httpd_uri_t& route : routes) httpd_register_uri_handler(g_server, &route);
  LOG_INF("BOOXLAN", "Server started on port %u", static_cast<unsigned>(config.server_port));

  const std::string host = tilescloud::mdnsHostname("crosspoint");
  MDNS.end();
  if (MDNS.begin(host.c_str())) {
    tilescloud::addMdnsService();
    g_mdns = true;
    LOG_INF("BOOXLAN", "mDNS: %s.local uniqueId=%s", host.c_str(), host.c_str() + strlen("Picco-"));
  } else {
    LOG_ERR("BOOXLAN", "mDNS failed");
  }
  updateStatus([](Status& s) { s = Status{}; });
  return true;
}

void stop() {
  if (!g_server) return;
  LOG_INF("BOOXLAN", "Stopping server...");
  httpd_stop(g_server);
  g_server = nullptr;
  if (g_mdns) {
    MDNS.end();
    g_mdns = false;
    LOG_INF("BOOXLAN", "mDNS stopped");
  }
  LOG_INF("BOOXLAN", "Server stopped");
}

bool running() { return g_server != nullptr; }

Status status() {
  portENTER_CRITICAL(&g_statusLock);
  Status copy = g_status;
  portEXIT_CRITICAL(&g_statusLock);
  return copy;
}

uint32_t statusVersion() { return g_statusVersion; }

}  // namespace booxlan

#pragma once

#include <cstddef>
#include <cstdint>

// The stock Picco firmware's "Import File" upload server, for the BOOX Tiles
// app: esp_http_server on port 80 with POST /upload?fileType=, OPTIONS
// /upload and /ping, HEAD /ping and GET /. One upload at a time; received
// data goes through three 32 KB PSRAM buffers to an SD writer task
// ("httpSd"), which writes it to the card mounted by booxsd.
namespace booxlan {

struct Status {
  int received = 0;
  int failed = 0;
  bool uploading = false;
  uint32_t uploadBytes = 0;
  uint32_t uploadTotal = 0;
  // Last result line, for the screen.
  char last[96] = "";
};

// Start the server and the stock mDNS service (Picco-<id>.local,
// _nanoreader._tcp). The card must be mounted with booxsd first.
bool start();
void stop();
bool running();
// A copy of the status, safe to call from the UI task.
Status status();
// Bumped whenever the status changes, so the screen knows to redraw.
uint32_t statusVersion();

}  // namespace booxlan

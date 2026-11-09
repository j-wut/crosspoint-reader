#pragma once

#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "network/TilesCloud.h"

// Runs a BOOX Tiles list sync, started from the list's sync row, (Wi-Fi join and HTTPS, up to a minute) on its
// own task so the list screen keeps answering input. The task works on a copy
// of the list; the screen takes the result when done. If the screen closes
// first, the task finishes on its own and frees its state.
template <typename Item>
class CloudSyncJob {
 public:
  using SyncFn = tilescloud::SyncResult (*)(std::vector<Item>&);

  bool start(const std::vector<Item>& items, SyncFn fn) {
    if (state) return false;
    auto next = std::shared_ptr<State>(new (std::nothrow) State());
    if (!next) return false;
    next->items = items;
    next->fn = fn;
    auto* handle = new (std::nothrow) std::shared_ptr<State>(next);
    if (!handle) return false;
    // TLS handshakes need a deep stack (the loop task they ran on has 24 KB).
    if (xTaskCreate(run, "cloudSync", 16384, handle, 1, nullptr) != pdPASS) {
      delete handle;
      return false;
    }
    state = std::move(next);
    return true;
  }

  bool running() const { return state != nullptr; }

  // Text for the sync row's value: progress, then the last outcome.
  const char* statusText() const {
    if (running()) return tr(STR_CLOUD_SYNCING);
    if (!finished) return nullptr;
    switch (last) {
      case tilescloud::SyncResult::Ok:
        return tr(STR_CLOUD_SYNC_DONE);
      case tilescloud::SyncResult::NoWifi:
        return tr(STR_CLOUD_SYNC_NO_WIFI);
      case tilescloud::SyncResult::NoBinding:
        return tr(STR_CLOUD_SYNC_NOT_BOUND);
      default:
        return tr(STR_CLOUD_SYNC_FAILED);
    }
  }

  // True once, when the task has finished; `items` gets the synced list.
  bool take(std::vector<Item>& items, tilescloud::SyncResult& result) {
    if (!state || !state->done.load(std::memory_order_acquire)) return false;
    result = state->result;
    last = result;
    finished = true;
    items = std::move(state->items);
    state.reset();
    return true;
  }

 private:
  struct State {
    std::vector<Item> items;
    SyncFn fn = nullptr;
    tilescloud::SyncResult result = tilescloud::SyncResult::Failed;
    std::atomic<bool> done{false};
  };

  static void run(void* arg) {
    auto* handle = static_cast<std::shared_ptr<State>*>(arg);
    State& s = **handle;
    s.result = s.fn(s.items);
    s.done.store(true, std::memory_order_release);
    delete handle;
    vTaskDelete(nullptr);
  }

  std::shared_ptr<State> state;
  tilescloud::SyncResult last = tilescloud::SyncResult::Skipped;
  bool finished = false;
};

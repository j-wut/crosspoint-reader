#pragma once
#include <Arduino.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstdlib>
#include <memory>
#include <new>

// Decoder objects are large (JPEGDEC ~17 KB, PNG ~42 KB). They go in the
// default heap when it has room. Boards with PSRAM use PSRAM when the internal
// heap is short, so a tight moment draws the image instead of an empty box.
template <typename T>
struct DecoderDeleter {
  void operator()(T* p) const {
    p->~T();
    heap_caps_free(p);
  }
};
template <typename T>
using DecoderPtr = std::unique_ptr<T, DecoderDeleter<T>>;

template <typename T>
DecoderPtr<T> allocateDecoder(const char* tag, const size_t minFreeHeap) {
  const size_t freeHeap = ESP.getFreeHeap();
  void* mem = freeHeap >= minFreeHeap ? malloc(sizeof(T)) : nullptr;
#if defined(BOARD_HAS_PSRAM)
  if (!mem) {
    mem = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem)
      LOG_INF(tag, "Decoder in PSRAM (heap %u free, wants %u)", static_cast<unsigned>(freeHeap),
              static_cast<unsigned>(minFreeHeap));
  }
#endif
  if (!mem) {
    LOG_ERR(tag, "Not enough memory for decoder (heap %u free, need %u)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(minFreeHeap));
    return nullptr;
  }
  return DecoderPtr<T>(new (mem) T());
}

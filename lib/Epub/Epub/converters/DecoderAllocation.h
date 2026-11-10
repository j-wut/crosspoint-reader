#pragma once
#include <Arduino.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstddef>

// True when a decoder can be allocated with headroom to spare. On boards with
// PSRAM, malloc puts blocks over SPIRAM_MALLOC_ALWAYSINTERNAL (4 KB) in PSRAM
// first, so free PSRAM counts as well as the internal heap.
inline bool decoderMemoryAvailable(const char* tag, const size_t needed) {
  const size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap >= needed) return true;
#if defined(BOARD_HAS_PSRAM)
  const size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (freePsram >= needed) return true;
#else
  const size_t freePsram = 0;
#endif
  LOG_ERR(tag, "Not enough memory for decoder (heap %u, PSRAM %u free, need %u)", static_cast<unsigned>(freeHeap),
          static_cast<unsigned>(freePsram), static_cast<unsigned>(needed));
  return false;
}

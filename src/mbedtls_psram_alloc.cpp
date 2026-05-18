#include "mbedtls_psram_alloc.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

namespace {

// PSRAM-first calloc. Falls back to default internal heap if PSRAM is
// exhausted so callers never see ENOMEM during normal operation.
void* psram_calloc(size_t nmemb, size_t size) {
    void* p = heap_caps_calloc(nmemb, size, MALLOC_CAP_SPIRAM);
    if (!p) {
        // PSRAM full (shouldn't happen with 8 MB available, but defensive).
        // Fall back to internal heap — the original mbedtls behaviour.
        p = heap_caps_calloc(nmemb, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

void psram_free(void* ptr) {
    // heap_caps_free figures out which heap the pointer came from.
    heap_caps_free(ptr);
}

bool g_initialized = false;

}  // namespace

void mbedtls_psram_alloc_init() {
    if (g_initialized) return;
    int rc = mbedtls_platform_set_calloc_free(psram_calloc, psram_free);
    if (rc == 0) {
        Serial.printf("[mbedtls] PSRAM-first allocator registered "
                      "(handshake buffers will live in PSRAM, not "
                      "internal DMA heap)\n");
        g_initialized = true;
    } else {
        Serial.printf("[mbedtls] platform_set_calloc_free failed rc=%d — "
                      "TLS will continue using internal heap (OOM risk)\n",
                      rc);
    }
}

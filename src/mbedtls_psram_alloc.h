#pragma once

// Route mbedtls heap allocations to PSRAM via mbedtls_platform_set_calloc_free.
//
// Rationale: KoSync TLS handshake from reader-context OOMs on the default
// 16 KB + 16 KB mbedtls record buffers because they're pinned in the tight
// internal-DMA-RAM heap (observed: dma_largest ~7-10 KB after WiFi.begin →
// mbedtls_ssl_setup returns -0x7F00 MBEDTLS_ERR_SSL_ALLOC_FAILED).
//
// We can't tune CONFIG_MBEDTLS_SSL_*_CONTENT_LEN at compile time because
// pioarduino's IDF rebuild via custom_sdkconfig conflicts with the
// arduino-as-component linker (__wrap_log_printf undefined refs). Instead
// register a PSRAM-first allocator at runtime — the buffers are still 16 KB
// each, but they live in PSRAM (8 MB available) instead of internal heap.
//
// Trade-off: ~10-50 ms extra TLS handshake latency from PSRAM access vs
// internal SRAM. For a once-per-sync handshake on an e-reader this is
// invisible to the user.
//
// Must be called once at setup() before any TLS-using code runs (kosync,
// OTA, ESP-TLS via WiFiClientSecure, etc.). Safe to call multiple times.
void mbedtls_psram_alloc_init();

// kosync_hash.cpp — KoReader-compatible partial-MD5 document hashing
//
// Upstream: koreader/frontend/util.lua :: util.partialMD5.
// Algorithm: read 1024-byte chunks at offsets `lshift(step, 2*i)` for
// i = -1..10. KOReader's runtime uses 32-bit modular shift, so i=-1
// resolves to offset 0; i >= 0 uses `step << (2*i)` → {0, 1024, 4096,
// 16384, …, 2^30}. All chunks feed ONE MD5 context (we buffer the ≤12 KiB
// concatenation and call the one-shot mbedtls_md5 once — see below for
// why streaming isn't available). Cross-reference: CrossInk
// lib/KOReaderSync/KOReaderDocumentId.cpp uses identical offset table
// against real kosync servers.

#include "kosync_hash.h"

#include <SD.h>
#include <esp_heap_caps.h>
#include <mbedtls/md5.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr size_t kChunkSize = 1024;
// KoReader's loop is `for i = -1, 10`, i.e. 12 iterations (i in {-1, 0, …, 10}).
// Upstream uses `lshift(step, 2*i)`; in 32-bit modular shift semantics (Lua
// bitop on KOReader's runtime), `lshift(1024, -2)` evaluates to 0. For i ≥ 0
// the offset is `step << (2*i)` → {0, 1024, 4096, 16384, …, 2^30}.
constexpr int kIterStart = -1;
constexpr int kIterEnd = 10;
// Max bytes we ever buffer: 12 iterations × 1024 B = 12 KiB. We use the
// one-shot `mbedtls_md5(input, len, output)` rather than the streaming
// _starts/_update/_finish API because espressif32@6.4.0 declares but does
// NOT link those wrappers (libmbedcrypto.a ships only `mbedtls_md5`).
constexpr size_t kMaxBufferedBytes = static_cast<size_t>(kChunkSize) *
                                     static_cast<size_t>(kIterEnd - kIterStart + 1);

uint64_t kosync_offset_for(int i) {
    if (i < 0) return 0;
    return static_cast<uint64_t>(kChunkSize) << (2 * i);
}

// Format a 16-byte MD5 digest as a 32-char lowercase hex String.
String digest_to_hex(const uint8_t (&digest)[16]) {
    char buf[33];
    for (int i = 0; i < 16; ++i) {
        snprintf(buf + (i * 2), 3, "%02x", digest[i]);
    }
    buf[32] = '\0';
    return String(buf);
}

} // namespace

String kosync_compute_document_hash(const String& epubFilePath) {
    if (epubFilePath.length() == 0) {
        Serial.println("[kosync_hash] empty path");
        return String();
    }

    File f = SD.open(epubFilePath.c_str(), FILE_READ);
    if (!f) {
        Serial.printf("[kosync_hash] open failed: %s\n", epubFilePath.c_str());
        return String();
    }

    const size_t fileSize = f.size();
    if (fileSize == 0) {
        Serial.printf("[kosync_hash] zero-sized file: %s\n", epubFilePath.c_str());
        f.close();
        return String();
    }

    // Buffer all chunks (≤12 KiB total) then hash once via mbedtls_md5().
    // espressif32@6.4.0's libmbedcrypto.a only links the one-shot variant;
    // the _starts/_update/_finish lifecycle wrappers are declared but not
    // implemented in the prebuilt binary.
    //
    // Allocate the buffer in PSRAM rather than on the stack or BSS:
    //   * Stack: 12 KiB out of a 16 KiB Arduino loop task stack leaves
    //     <4 KiB for the surrounding call chain → overflow risk under
    //     deep stacks (sync → reader → parser → hash).
    //   * BSS (`static`): permanently consumes internal SRAM, fragmenting
    //     the DMA-capable heap that WiFi/TLS need (observed dma_largest
    //     collapse to <1.5 KiB after WiFi.begin, then DNS getaddrinfo
    //     fails with EAI_FAIL).
    // PSRAM has plenty of room (multi-MB) and isn't DMA-capable anyway,
    // so it's the right pool for a transient compute scratch buffer.
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(kMaxBufferedBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        // PSRAM exhausted (or board configured without PSRAM). Fall back
        // to the regular heap before giving up so the diagnostic endpoint
        // and library-scan paths still work on non-PSRAM builds.
        buf = static_cast<uint8_t*>(malloc(kMaxBufferedBytes));
    }
    if (!buf) {
        Serial.println("[kosync_hash] buffer allocation failed");
        f.close();
        return String();
    }
    size_t bufLen = 0;
    bool readError = false;

    for (int i = kIterStart; i <= kIterEnd; ++i) {
        const uint64_t offset = kosync_offset_for(i);
        // Offsets are non-decreasing for i ≥ 0; `continue` and `break`
        // are equivalent in practice but `continue` is safer if an
        // additional negative index is ever prepended to the table.
        if (offset >= fileSize) continue;

        // ESP32-Arduino's File::seek() takes uint32_t. Offsets up to
        // `kChunkSize << (2 * kIterEnd)` = 1 GiB at kIterEnd=10 fit; bumping
        // kIterEnd past 10 would silently truncate here — revisit the cast
        // (and the seek API) before raising the table.
        if (!f.seek(static_cast<uint32_t>(offset))) {
            Serial.printf("[kosync_hash] seek failed at offset %u\n",
                          (unsigned)offset);
            readError = true;
            break;
        }

        const size_t remaining = fileSize - static_cast<size_t>(offset);
        const size_t want = remaining < kChunkSize ? remaining : kChunkSize;
        const int got = f.read(buf + bufLen, want);
        if (got <= 0) {
            Serial.printf("[kosync_hash] read failed at offset %u (got=%d)\n",
                          (unsigned)offset, got);
            readError = true;
            break;
        }
        // Short read produces a silently-wrong digest (mismatched chunk
        // boundary feeds the MD5 fewer bytes than KOReader does). FATFS
        // on a healthy SD always fills the buffer when enough bytes
        // remain, so a short read indicates a real fault — abort and
        // let getDocumentHash() refuse to cache the partial result.
        if (static_cast<size_t>(got) != want) {
            Serial.printf("[kosync_hash] short read at offset %u (got=%d, want=%u)\n",
                          (unsigned)offset, got, (unsigned)want);
            readError = true;
            break;
        }
        bufLen += static_cast<size_t>(got);
    }

    f.close();

    if (readError || bufLen == 0) {
        free(buf);
        return String();
    }

    uint8_t digest[16];
    const int rc = mbedtls_md5(buf, bufLen, digest);
    free(buf);
    if (rc != 0) {
        Serial.println("[kosync_hash] mbedtls_md5 failed");
        return String();
    }

    return digest_to_hex(digest);
}

bool kosync_hash_is_valid(const String& epubFilePath, size_t cachedEpubSize) {
    if (epubFilePath.length() == 0 || cachedEpubSize == 0) {
        return false;
    }

    File f = SD.open(epubFilePath.c_str(), FILE_READ);
    if (!f) {
        return false;
    }

    const size_t currentSize = f.size();
    f.close();

    return currentSize == cachedEpubSize;
}

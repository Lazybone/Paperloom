// kosync_hash.cpp — KoReader-compatible partial-MD5 document hashing
//
// Reference: KoReader v2025.x (latest stable at implementation time, see
// docs/kosync.md for pinned SHA).
// Upstream: koreader/frontend/util.lua :: util.partialMD5
//
// TODO(WP-14 acceptance): pin the exact upstream commit SHA and verify the
// step pattern byte-for-byte against `koreader/frontend/util.lua`. The
// canonical algorithm reads up to 11 chunks of 1024 bytes at offsets
// (step << (i+1)) for i = -1..10, feeds them all into ONE MD5 context,
// and returns the lowercase hex digest. We mirror that below; if the
// upstream ever changes the offset pattern we MUST update this file to
// match — kosync uses the digest as the document identity key and a
// divergent algorithm silently breaks cross-device sync.

#include "kosync_hash.h"

#include <SD.h>
#include <mbedtls/md5.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr size_t kChunkSize = 1024;
// KoReader's loop is `for i = -1, 10`, i.e. 12 iterations (i in {-1, 0, …, 10}).
// Offsets are step << (i+1) → {0, 2048, 4096, 8192, …, 2^21}.
constexpr int kIterStart = -1;
constexpr int kIterEnd = 10;
// Max bytes we ever buffer: 12 iterations × 1024 B = 12 KiB. We use the
// one-shot `mbedtls_md5(input, len, output)` rather than the streaming
// _starts/_update/_finish API because espressif32@6.4.0 declares but does
// NOT link those wrappers (libmbedcrypto.a ships only `mbedtls_md5`).
constexpr size_t kMaxBufferedBytes = static_cast<size_t>(kChunkSize) *
                                     static_cast<size_t>(kIterEnd - kIterStart + 1);

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
    uint8_t buf[kMaxBufferedBytes];
    size_t bufLen = 0;
    bool readError = false;

    for (int i = kIterStart; i <= kIterEnd; ++i) {
        const uint64_t offset = static_cast<uint64_t>(kChunkSize) << (i + 1);
        if (offset >= fileSize) break;

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
        bufLen += static_cast<size_t>(got);
    }

    f.close();

    if (readError || bufLen == 0) {
        return String();
    }

    uint8_t digest[16];
    if (mbedtls_md5_ret(buf, bufLen, digest) != 0) {
        Serial.println("[kosync_hash] mbedtls_md5_ret failed");
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

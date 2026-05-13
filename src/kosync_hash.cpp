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

// RAII guard for the mbedtls MD5 context so every early-return path
// releases the context cleanly.
class Md5Guard {
public:
    Md5Guard() { mbedtls_md5_init(&ctx_); }
    ~Md5Guard() { mbedtls_md5_free(&ctx_); }
    Md5Guard(const Md5Guard&) = delete;
    Md5Guard& operator=(const Md5Guard&) = delete;
    mbedtls_md5_context* get() { return &ctx_; }

private:
    mbedtls_md5_context ctx_;
};

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

    Md5Guard md5;
    // ESP-IDF 5.x (espressif32 6.4.0) ships the modern non-_ret mbedtls API.
    // The older *_ret variants were removed when the deprecated wrappers were
    // dropped; the modern variants return void — no status to check.
    mbedtls_md5_starts(md5.get());

    uint8_t buf[kChunkSize];
    bool readError = false;

    // Mirror KoReader's util.partialMD5 loop: `for i = -1, 10`.
    // offset = step << (i+1) → 0, 2K, 4K, 8K, 16K, 32K, …, 2 MiB.
    for (int i = kIterStart; i <= kIterEnd; ++i) {
        const uint64_t offset = static_cast<uint64_t>(kChunkSize) << (i + 1);
        if (offset >= fileSize) {
            // Past EOF: stop feeding chunks (KoReader breaks here too).
            break;
        }

        if (!f.seek(static_cast<uint32_t>(offset))) {
            Serial.printf("[kosync_hash] seek failed at offset %u\n",
                          (unsigned)offset);
            readError = true;
            break;
        }

        // Read up to kChunkSize bytes; the final chunk may be short if the
        // remaining file is smaller than 1024 B.
        const size_t remaining = fileSize - static_cast<size_t>(offset);
        const size_t want = remaining < kChunkSize ? remaining : kChunkSize;
        const int got = f.read(buf, want);
        if (got <= 0) {
            Serial.printf("[kosync_hash] read failed at offset %u (got=%d)\n",
                          (unsigned)offset, got);
            readError = true;
            break;
        }

        mbedtls_md5_update(md5.get(), buf, static_cast<size_t>(got));
    }

    f.close();

    if (readError) {
        return String();
    }

    uint8_t digest[16];
    mbedtls_md5_finish(md5.get(), digest);

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

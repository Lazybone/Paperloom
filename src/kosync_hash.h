#pragma once
#include <Arduino.h>
#include <stddef.h>

// Identifier for the partial-MD5 offset table used by
// kosync_compute_document_hash(). Bump whenever the algorithm changes so
// reader.cpp can invalidate stale cached digests from older firmware.
// Persisted in progress JSON as "kosync_hash_algo".
//
// History:
//   1 — legacy `step << (i+1)` offset table (incompatible with KOReader).
//   2 — KOReader-compatible `step << (2*i)` with i=-1 → offset 0.
constexpr int kKosyncHashAlgoVersion = 2;

// Compute KoReader-compatible partial-MD5 of the EPUB file at the given path.
// Returns 32-char lowercase hex digest, or empty String on file-open error.
// Streaming reads only (≤12 KB total), so safe on tight heap.
String kosync_compute_document_hash(const String& epubFilePath);

// Returns true iff the file at `epubFilePath` still has size `cachedEpubSize`.
// Used to decide whether a previously-stored hash is still valid.
bool kosync_hash_is_valid(const String& epubFilePath, size_t cachedEpubSize);

#pragma once
#include <Arduino.h>
#include <stddef.h>

// Compute KoReader-compatible partial-MD5 of the EPUB file at the given path.
// Returns 32-char lowercase hex digest, or empty String on file-open error.
// Streaming reads only (≤11 KB total), so safe on tight heap.
String kosync_compute_document_hash(const String& epubFilePath);

// Returns true iff the file at `epubFilePath` still has size `cachedEpubSize`.
// Used to decide whether a previously-stored hash is still valid.
bool kosync_hash_is_valid(const String& epubFilePath, size_t cachedEpubSize);

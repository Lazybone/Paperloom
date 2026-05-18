#pragma once

#include <Arduino.h>

// Shared validators for kosync identifiers. Inline so each TU gets its own
// definition (no linker collision) while the source of truth lives in this
// single header — previously the same logic existed verbatim in
// kosync_client.cpp and kosync_http_handlers.cpp and could drift.

namespace kosync_validation {

// Lowercase-hex of fixed length. Used for the 32-char MD5 password hash and
// the 32-char document MD5 hash. Case-sensitive on purpose — the kosync
// wire format is canonical lowercase.
inline bool is_lowercase_hex(const String& s, size_t expected_len) {
    if (s.length() != expected_len) return false;
    for (size_t i = 0; i < expected_len; ++i) {
        const char c = s.charAt(i);
        const bool digit = (c >= '0' && c <= '9');
        const bool hex   = (c >= 'a' && c <= 'f');
        if (!digit && !hex) return false;
    }
    return true;
}

// Username charset matches the kosync server: [A-Za-z0-9_.-], length 1..32.
// Both the local KosyncClient and the on-device registration handler use
// the same charset, so divergence here would cause a credential entered in
// one path to fail validation in the other.
inline bool is_valid_username(const String& s) {
    const size_t n = s.length();
    if (n < 1 || n > 32) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s.charAt(i);
        const bool ok =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

}  // namespace kosync_validation

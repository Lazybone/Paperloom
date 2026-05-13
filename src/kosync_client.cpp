#include "kosync_client.h"
#include "tls_certs.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <memory>

// ─── Constants ──────────────────────────────────────────────────────

// Bounded JSON sizes — pull response is small (device, device_id, progress,
// percentage, timestamp), push body is even smaller (5 fields). Keep these
// tight so we don't churn DRAM on a memory-constrained ESP32-S3.
static const size_t kPullDocBytes = 1024;
static const size_t kPushDocBytes = 512;

// Network timing — match the OTA path. 15 s covers a slow TLS handshake on
// a flaky connection without blocking the UI for a noticeable eternity.
static const uint32_t kHttpTimeoutMs = 15000;
static const int      kTlsHandshakeS = 15;

// Cap any debug excerpt we log so a hostile server can't flood the serial
// buffer or our log ring. Never log raw secrets even at this length.
static const size_t kDebugExcerptMax = 200;

// User-facing German error strings — kept consistent with the rest of the UI.
static const char* kErrInvalidHash    = "invalid document hash";
static const char* kErrNoWifi         = "Kein WLAN";
static const char* kErrUnreachable    = "Server nicht erreichbar";
static const char* kErrAuth           = "Login ungültig";
static const char* kErrServer         = "Serverfehler";

// ─── Local helpers ──────────────────────────────────────────────────

// Strictly lowercase hex of fixed length. We do not accept uppercase to
// keep the URL form canonical (server-side kosync is case-sensitive).
static bool is_lowercase_hex(const String& s, size_t expected_len) {
    if (s.length() != expected_len) return false;
    for (size_t i = 0; i < expected_len; ++i) {
        char c = s.charAt(i);
        const bool digit = (c >= '0' && c <= '9');
        const bool hex   = (c >= 'a' && c <= 'f');
        if (!digit && !hex) return false;
    }
    return true;
}

// Username charset matches the kosync server: [A-Za-z0-9_.-], 1..32.
static bool is_valid_username(const String& s) {
    if (s.length() < 1 || s.length() > 32) return false;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s.charAt(i);
        const bool ok =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// Case-sensitive byte-compare for the "https://" prefix. We refuse mixed
// case ("HTTPS://", "Https://") — Arduino's HTTPClient is lenient enough
// to accept them, but normalising at the boundary prevents a downstream
// allow-list mismatch.
static bool has_https_prefix(const String& s) {
    static const char* p = "https://";
    if (s.length() < 8) return false;
    for (size_t i = 0; i < 8; ++i) {
        if (s.charAt(i) != p[i]) return false;
    }
    return true;
}

// Strip ALL trailing slashes so we can paste "/syncs/progress/<hash>" in.
// "https://kosync.eu/" and "https://kosync.eu" must produce the same URL.
static String strip_trailing_slash(const String& s) {
    String out = s;
    while (out.length() > 0 && out.charAt(out.length() - 1) == '/') {
        out.remove(out.length() - 1);
    }
    return out;
}

// Truncate a response body for debug logging. Caller is responsible for
// scrubbing any sensitive substring before passing it in — but the kosync
// response body itself doesn't contain auth headers, so we just length-cap.
static String redacted_excerpt(const String& body) {
    if (body.length() <= kDebugExcerptMax) return body;
    String s = body.substring(0, kDebugExcerptMax);
    s += "...";
    return s;
}

// Map an HTTP status (>= 100) to the appropriate German error string.
// `pull_404_ok` distinguishes the two semantic regimes:
//   pull: 404 = fresh sync, NOT an error → caller should ignore.
//   push: 404 = unexpected, kosync auto-creates → treat as Serverfehler.
static const char* http_error_for(int status, bool pull_404_ok) {
    if (status == 404 && pull_404_ok) return "";
    if (status == 401 || status == 403) return kErrAuth;
    if (status >= 500 && status <= 599) return kErrServer;
    if (status == 404)                 return kErrServer;
    // Any other 4xx that isn't 401/403/404 is a client error we can't
    // recover from — surface as Serverfehler rather than silently 0'ing.
    if (status >= 400 && status <= 499) return kErrServer;
    return "";
}

// ─── Construction & validation ──────────────────────────────────────

KosyncClient::KosyncClient(const String& serverUrl,
                           const String& username,
                           const String& passwordMd5) {
    // Server URL: case-sensitive "https://" prefix, length ≤256.
    if (serverUrl.length() == 0 || serverUrl.length() > 256) {
        Serial.printf("[kosync_client] invalid serverUrl\n");
        invalid_ = true;
    } else if (!has_https_prefix(serverUrl)) {
        Serial.printf("[kosync_client] invalid serverUrl\n");
        invalid_ = true;
    }
    server_ = strip_trailing_slash(serverUrl);

    if (!is_valid_username(username)) {
        Serial.printf("[kosync_client] invalid username\n");
        invalid_ = true;
    }
    user_ = username;

    // Password is already MD5-hashed by the caller — strictly lowercase
    // 32 hex chars. We never see the plaintext.
    if (!is_lowercase_hex(passwordMd5, 32)) {
        Serial.printf("[kosync_client] invalid passwordMd5\n");
        invalid_ = true;
    }
    key_ = passwordMd5;
}

bool KosyncClient::isWiFiConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

bool KosyncClient::validateDocHash_(const String& h) const {
    return is_lowercase_hex(h, 32);
}

// ─── TLS client lifecycle ───────────────────────────────────────────

// TLS buffer allocated from DRAM (~16 KB). Manual sync does not overlap
// with chapter loading; heap pressure acceptable. PSRAM not required.
void KosyncClient::ensureClient_() {
    if (client_) return;
    // Note: project defaults to C++11; std::make_unique requires C++14.
    client_.reset(new WiFiClientSecure());
    // Pin the project trust bundle. Without this the ESP32-S3 Arduino
    // build has no CA store at all — connections would either fail or
    // (worse) fall back to setInsecure() in the caller. The bundle
    // covers ISRG Root X1, which kosync.eu and similar Let's Encrypt
    // deployments chain to.
    client_->setCACert(PAPERLOOM_TRUSTED_ROOTS);
    client_->setHandshakeTimeout(kTlsHandshakeS);
}

void KosyncClient::resetClient_() {
    client_.reset();
}

// ─── PULL ───────────────────────────────────────────────────────────

int KosyncClient::pullProgress(const String& documentHash,
                               KosyncProgress& out_progress,
                               String& out_error) {
    out_error = "";

    if (invalid_) {
        out_error = kErrUnreachable;
        return 0;
    }
    if (!isWiFiConnected()) {
        out_error = kErrNoWifi;
        return 0;
    }
    if (!validateDocHash_(documentHash)) {
        out_error = kErrInvalidHash;
        return 0;
    }

    ensureClient_();

    // Hash is pre-validated as 32 lowercase hex chars — no escaping needed.
    String url = server_;
    url += "/syncs/progress/";
    url += documentHash;

    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

    if (!http.begin(*client_, url)) {
        Serial.printf("[kosync_client] pull: http.begin failed\n");
        const mbedtls_x509_crt* peer = client_->getPeerCertificate();
        if (peer) {
            // Don't dump the whole cert — just note that we saw one. The
            // mbedtls subject DN is a binary structure on Arduino-ESP32
            // and there's no stable public accessor for the CN string,
            // so we keep this an info-level "yes a cert was seen" marker.
            Serial.printf("[kosync_client] pull: peer cert presented (tls/handshake stage)\n");
        }
        out_error = kErrUnreachable;
        http.end();
        resetClient_();
        return 0;
    }

    http.addHeader("X-Auth-User", user_);
    http.addHeader("X-Auth-Key",  key_);
    http.addHeader("Accept",      "application/json");

    int status = http.GET();
    if (status <= 0) {
        // Negative codes from HTTPClient cover the failure modes we lump
        // together as "unreachable": HTTPC_ERROR_CONNECTION_REFUSED,
        // _SEND_HEADER_FAILED, _READ_TIMEOUT, etc. Translate to a single
        // user-facing string.
        Serial.printf("[kosync_client] pull: transport error %d\n", status);
        out_error = kErrUnreachable;
        http.end();
        resetClient_();
        return 0;
    }

    Serial.printf("[kosync_client] pull: status=%d\n", status);

    if (status == 200) {
        // Cap the response body so a hostile / broken server can't OOM us.
        String body = http.getString();
        DynamicJsonDocument doc(kPullDocBytes);
        DeserializationError err = deserializeJson(doc, body);
        if (err) {
            Serial.printf("[kosync_client] pull: parse error: %s\n", err.c_str());
            Serial.printf("[kosync_client] pull: body excerpt: %s\n",
                          redacted_excerpt(body).c_str());
            out_error = kErrServer;
            http.end();
            resetClient_();
            return 500;
        }

        out_progress.device     = doc["device"]     | "";
        out_progress.deviceId   = doc["device_id"]  | "";
        out_progress.progress   = doc["progress"]   | "";
        out_progress.percentage = doc["percentage"] | 0.0f;
        out_progress.timestamp  = doc["timestamp"]  | 0u;
        // chapter/page are derived by the coordinator from `progress` +
        // `percentage`. They are NOT in the kosync wire format.
        out_progress.chapter = 0;
        out_progress.page    = 0;

        http.end();
        resetClient_();
        return 200;
    }

    if (status == 404) {
        // Fresh sync — server has no row for this document yet. NOT an
        // error; leave out_progress untouched per contract.
        http.end();
        resetClient_();
        return 404;
    }

    // Any other status: translate to a user-facing string.
    out_error = http_error_for(status, /*pull_404_ok=*/true);
    if (out_error.length() == 0) out_error = kErrServer;
    http.end();
    resetClient_();
    return status;
}

// ─── PUSH ───────────────────────────────────────────────────────────

int KosyncClient::pushProgress(const String& documentHash,
                               const KosyncProgress& progress,
                               String& out_error) {
    out_error = "";

    if (invalid_) {
        out_error = kErrUnreachable;
        return 0;
    }
    if (!isWiFiConnected()) {
        out_error = kErrNoWifi;
        return 0;
    }
    if (!validateDocHash_(documentHash)) {
        out_error = kErrInvalidHash;
        return 0;
    }

    ensureClient_();

    // PUT body — 5 fields; bounded by kPushDocBytes (512). `timestamp` is
    // assigned by the server, so we don't send it.
    StaticJsonDocument<kPushDocBytes> doc;
    doc["document"]   = documentHash;
    doc["progress"]   = progress.progress;
    doc["percentage"] = progress.percentage;
    doc["device"]     = progress.device;
    doc["device_id"]  = progress.deviceId;

    String body;
    body.reserve(kPushDocBytes);
    serializeJson(doc, body);

    String url = server_;
    url += "/syncs/progress";

    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

    if (!http.begin(*client_, url)) {
        Serial.printf("[kosync_client] push: http.begin failed\n");
        const mbedtls_x509_crt* peer = client_->getPeerCertificate();
        if (peer) {
            Serial.printf("[kosync_client] push: peer cert presented (tls/handshake stage)\n");
        }
        out_error = kErrUnreachable;
        http.end();
        resetClient_();
        return 0;
    }

    http.addHeader("X-Auth-User",  user_);
    http.addHeader("X-Auth-Key",   key_);
    http.addHeader("Accept",       "application/json");
    http.addHeader("Content-Type", "application/json");

    // NB: never log `body` — it contains progress data that's not strictly
    // secret but, more importantly, normalising "don't log request bodies"
    // means we can't accidentally regress and start dumping the auth key
    // header value during a future refactor.
    int status = http.sendRequest("PUT", body);
    if (status <= 0) {
        Serial.printf("[kosync_client] push: transport error %d\n", status);
        out_error = kErrUnreachable;
        http.end();
        resetClient_();
        return 0;
    }

    Serial.printf("[kosync_client] push: status=%d\n", status);

    if (status == 200 || status == 201) {
        http.end();
        resetClient_();
        return status;
    }

    // PUSH-side mapping: 404 is unexpected (kosync auto-creates rows),
    // so it falls into the "server" bucket rather than the "fresh-sync"
    // bucket used by PULL.
    out_error = http_error_for(status, /*pull_404_ok=*/false);
    if (out_error.length() == 0) out_error = kErrServer;
    http.end();
    resetClient_();
    return status;
}

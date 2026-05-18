#include "kosync_client.h"

#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>

// ─── Constants ──────────────────────────────────────────────────────

// Small TLS buffer to fit in constrained reader-context heap. WiFiClientSecure
// defaults to 16 KB (the mbedtls compile-time default). esp_http_client exposes
// this as a runtime config — 2 KB is enough for <1 KB KOSync JSON payloads and
// avoids OOM during the TLS handshake when called from reader-context.
static const int    kHttpBufSize  = 2048;
static const uint32_t kTimeoutMs = 15000;

// Bounded JSON sizes — pull response is small (device, device_id, progress,
// percentage, timestamp), push body is even smaller (5 fields).
static const size_t kPullDocBytes = 1024;
static const size_t kPushDocBytes = 512;

// Cap any debug excerpt we log so a hostile server can't flood the serial
// buffer or our log ring. Never log raw secrets even at this length.
static const size_t kDebugExcerptMax = 200;

// User-facing German error strings — kept consistent with the rest of the UI.
static const char* kErrInvalidHash    = "invalid document hash";
static const char* kErrNoWifi         = "Kein WLAN";
static const char* kErrUnreachable    = "Server nicht erreichbar";
static const char* kErrAuth           = "Login ungültig";
static const char* kErrServer         = "Serverfehler";

// ─── Internal response buffer ───────────────────────────────────────

namespace {

// Growable heap buffer for collecting the HTTP response body. Freed in dtor.
struct ResponseBuffer {
    char* data     = nullptr;
    int   len      = 0;
    int   capacity = 0;

    ~ResponseBuffer() { free(data); }

    bool ensure(int size) {
        if (size <= capacity) return true;
        char* nd = (char*)realloc(data, size);
        if (!nd) return false;
        data = nd;
        capacity = size;
        return true;
    }
};

esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
        if (buf->ensure(buf->len + evt->data_len + 1)) {
            memcpy(buf->data + buf->len, evt->data, evt->data_len);
            buf->len += evt->data_len;
            buf->data[buf->len] = '\0';
        }
    }
    return ESP_OK;
}

}  // namespace

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

// Case-sensitive byte-compare for the "https://" prefix.
static bool has_https_prefix(const String& s) {
    static const char* p = "https://";
    if (s.length() < 8) return false;
    for (size_t i = 0; i < 8; ++i) {
        if (s.charAt(i) != p[i]) return false;
    }
    return true;
}

// Strip ALL trailing slashes so we can paste "/syncs/progress/<hash>" in.
static String strip_trailing_slash(const String& s) {
    String out = s;
    while (out.length() > 0 && out.charAt(out.length() - 1) == '/') {
        out.remove(out.length() - 1);
    }
    return out;
}

// Truncate a response body for debug logging.
static String redacted_excerpt(const char* body, int len) {
    if (!body || len == 0) return String("(empty)");
    size_t cap = (size_t)len < kDebugExcerptMax ? (size_t)len : kDebugExcerptMax;
    String s = String(body).substring(0, cap);
    if ((size_t)len > kDebugExcerptMax) s += "...";
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
    if (status >= 400 && status <= 499) return kErrServer;
    return "";
}

// ─── Construction & validation ──────────────────────────────────────

KosyncClient::KosyncClient(const String& serverUrl,
                           const String& username,
                           const String& passwordMd5) {
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

    // Hash is pre-validated as 32 lowercase hex chars — no escaping needed.
    String url = server_ + "/syncs/progress/" + documentHash;

    ResponseBuffer buf;
    esp_http_client_config_t config = {};
    config.url               = url.c_str();
    config.event_handler     = http_event_handler;
    config.user_data         = &buf;
    config.method            = HTTP_METHOD_GET;
    config.timeout_ms        = kTimeoutMs;
    config.buffer_size       = kHttpBufSize;
    config.buffer_size_tx    = kHttpBufSize;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        Serial.printf("[kosync_client] pull: esp_http_client_init failed\n");
        out_error = kErrUnreachable;
        return 0;
    }

    esp_http_client_set_header(client, "X-Auth-User", user_.c_str());
    esp_http_client_set_header(client, "X-Auth-Key",  key_.c_str());
    esp_http_client_set_header(client, "Accept",      "application/json");

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);

    if (err != ESP_OK) {
        Serial.printf("[kosync_client] pull: transport error %d (%s)\n",
                      err, esp_err_to_name(err));
        out_error = kErrUnreachable;
        esp_http_client_cleanup(client);
        return 0;
    }

    Serial.printf("[kosync_client] pull: status=%d\n", status);

    if (status == 200 && buf.data && buf.len > 0) {
        // Cap the response body so a hostile / broken server can't OOM us.
        DynamicJsonDocument doc(kPullDocBytes);
        DeserializationError jerr = deserializeJson(doc, buf.data, buf.len);
        if (jerr) {
            Serial.printf("[kosync_client] pull: JSON parse error: %s\n", jerr.c_str());
            Serial.printf("[kosync_client] pull: body excerpt: %s\n",
                          redacted_excerpt(buf.data, buf.len).c_str());
            out_error = kErrServer;
            esp_http_client_cleanup(client);
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

        esp_http_client_cleanup(client);
        return 200;
    }

    if (status == 404) {
        // Fresh sync — server has no row for this document yet. NOT an
        // error; leave out_progress untouched per contract.
        esp_http_client_cleanup(client);
        return 404;
    }

    // Any other status: translate to a user-facing string.
    out_error = http_error_for(status, /*pull_404_ok=*/true);
    if (out_error.length() == 0) out_error = kErrServer;
    esp_http_client_cleanup(client);
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

    String url = server_ + "/syncs/progress";

    ResponseBuffer buf;
    esp_http_client_config_t config = {};
    config.url               = url.c_str();
    config.event_handler     = http_event_handler;
    config.user_data         = &buf;
    config.method            = HTTP_METHOD_PUT;
    config.timeout_ms        = kTimeoutMs;
    config.buffer_size       = kHttpBufSize;
    config.buffer_size_tx    = kHttpBufSize;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        Serial.printf("[kosync_client] push: esp_http_client_init failed\n");
        out_error = kErrUnreachable;
        return 0;
    }

    esp_http_client_set_header(client, "X-Auth-User",  user_.c_str());
    esp_http_client_set_header(client, "X-Auth-Key",   key_.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // NB: never log `body` — it contains progress data that's not strictly
    // secret but, more importantly, normalising "don't log request bodies"
    // means we can't accidentally regress and start dumping the auth key
    // header value during a future refactor.
    esp_http_client_set_post_field(client, body.c_str(), body.length());

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);

    if (err != ESP_OK) {
        Serial.printf("[kosync_client] push: transport error %d (%s)\n",
                      err, esp_err_to_name(err));
        out_error = kErrUnreachable;
        esp_http_client_cleanup(client);
        return 0;
    }

    Serial.printf("[kosync_client] push: status=%d\n", status);

    if (status == 200 || status == 201) {
        esp_http_client_cleanup(client);
        return status;
    }

    // PUSH-side mapping: 404 is unexpected (kosync auto-creates rows),
    // so it falls into the "server" bucket rather than the "fresh-sync"
    // bucket used by PULL.
    out_error = http_error_for(status, /*pull_404_ok=*/false);
    if (out_error.length() == 0) out_error = kErrServer;
    esp_http_client_cleanup(client);
    return status;
}

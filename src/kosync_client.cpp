#include "kosync_client.h"

#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>

#include "tls_certs.h"
#include "kosync_validation.h"

// ─── Constants ──────────────────────────────────────────────────────

// HTTP response receive buffer for esp_http_client. NOTE: this is the
// HTTP body buffer, NOT the mbedtls TLS record buffer. The TLS record
// buffer is sized at compile time via sdkconfig:
//   CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN  (default 16 KB, allocated from
//   CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN  internal heap during handshake)
// Reducing TLS handshake heap pressure requires either tuning those
// sdkconfig values, enabling CONFIG_MBEDTLS_DYNAMIC_BUFFER, or routing
// mbedtls allocations to PSRAM via CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC.
// Plan H (BookReader::releaseForSync) frees the EPUB parser before the
// handshake to make room; that is the current mitigation.
static constexpr int      kHttpBufSize     = 2048;
static constexpr uint32_t kTimeoutMs       = 15000;

// Bounded JSON sizes — pull response is small (device, device_id, progress,
// percentage, timestamp), push body is even smaller (5 fields).
static constexpr size_t kPullDocBytes      = 1024;
static constexpr size_t kPushDocBytes      = 512;

// Hard ceiling on accumulated HTTP response body to prevent a hostile
// server from OOM'ing the device via unbounded streaming. KoSync pull
// responses are <1 KB in practice; 4 KB leaves generous slack.
static constexpr int    kResponseHardCap   = 4096;

// Cap any debug excerpt we log so a hostile server can't flood the serial
// buffer or our log ring. Never log raw secrets even at this length.
static constexpr size_t kDebugExcerptMax   = 200;

// User-facing German error strings — kept consistent with the rest of the UI.
static constexpr const char* kErrInvalidHash = "invalid document hash";
static constexpr const char* kErrNoWifi      = "Kein WLAN";
static constexpr const char* kErrUnreachable = "Server nicht erreichbar";
static constexpr const char* kErrAuth        = "Login ungültig";
static constexpr const char* kErrServer      = "Serverfehler";

// ─── Internal response buffer ───────────────────────────────────────

namespace {

// Growable heap buffer for collecting the HTTP response body. Pre-allocated
// to 1 KB at construction so the typical KoSync pull response (<1 KB) needs
// no realloc during the TLS event callback — important because realloc on a
// fragmented internal heap can fragment it further while mbedtls is still
// holding handshake buffers. `truncated` is set sticky on either OOM
// (realloc fail) or hard-cap overflow; callers MUST inspect it before
// trusting any parsed payload.
struct ResponseBuffer {
    char* data      = nullptr;
    int   len       = 0;
    int   capacity  = 0;
    bool  truncated = false;

    ResponseBuffer() {
        data = static_cast<char*>(malloc(1024));
        if (data) {
            capacity = 1024;
            data[0]  = '\0';
        } else {
            // Pre-alloc failed (severe internal-heap OOM). Mark sticky so
            // the event handler skips any data and the caller refuses to
            // parse the (empty) body. Without this flag the handler would
            // attempt realloc(nullptr,…) and only flip truncated on the
            // second failure — one callback too late.
            truncated = true;
        }
    }
    ~ResponseBuffer() { free(data); }

    bool ensure(int size) {
        if (size <= capacity) return true;
        if (size >= kResponseHardCap) {
            truncated = true;
            return false;
        }
        int newCap = capacity ? capacity : 256;
        while (newCap < size) newCap *= 2;
        if (newCap > kResponseHardCap) newCap = kResponseHardCap;
        char* nd = static_cast<char*>(realloc(data, newCap));
        if (!nd) {
            truncated = true;
            return false;
        }
        data     = nd;
        capacity = newCap;
        return true;
    }
};

esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf && !buf->truncated) {
        if (buf->ensure(buf->len + evt->data_len + 1)) {
            memcpy(buf->data + buf->len, evt->data, evt->data_len);
            buf->len += evt->data_len;
            buf->data[buf->len] = '\0';
        }
        // If ensure() returned false, `truncated` is now set and any
        // subsequent ON_DATA chunks are skipped via the guard above.
    }
    return ESP_OK;
}

// Shared validators (is_lowercase_hex, is_valid_username) live in
// kosync_validation.h so the registration handler in kosync_http_handlers.cpp
// uses the same definitions. Bring them into this TU's anonymous-namespace
// scope so the names do NOT leak into the global namespace.
using kosync_validation::is_lowercase_hex;
using kosync_validation::is_valid_username;

}  // namespace

// ─── Local helpers ──────────────────────────────────────────────────

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
    config.url                  = url.c_str();
    config.event_handler        = http_event_handler;
    config.user_data            = &buf;
    config.method               = HTTP_METHOD_GET;
    config.timeout_ms           = kTimeoutMs;
    config.buffer_size          = kHttpBufSize;
    config.buffer_size_tx       = kHttpBufSize;
    // Cert pinning: trust the same narrow root set as the rest of Paperloom
    // (USERTrust ECC + ISRG Root X1 + DigiCert Global Root CA). The Mozilla
    // crt_bundle would trust ~140 CAs — excessive for a user-configured
    // sync server and a broader MITM surface than necessary.
    config.cert_pem             = PAPERLOOM_TRUSTED_ROOTS;
    // Refuse to follow redirects so X-Auth-User / X-Auth-Key never leak to
    // a different host. A misconfigured / hostile server returning 3xx
    // would otherwise see the auth headers re-sent at the redirect target.
    config.disable_auto_redirect = true;

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

    // Refuse to trust a body we may have truncated mid-stream — a partial
    // JSON could parse to defaulted values (e.g. percentage=0.0) and then
    // get pushed back to the server, silently overwriting real progress.
    if (buf.truncated) {
        Serial.printf("[kosync_client] pull: response truncated (cap=%d)\n",
                      kResponseHardCap);
        out_error = kErrServer;
        esp_http_client_cleanup(client);
        return 500;
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

        // Defense-in-depth normalization at the parse boundary so any
        // future caller that touches these fields before bounds_ok() runs
        // still sees sane values.
        // - percentage clamped to [0.0, 1.0]
        // - progress string capped at 128 chars (parse_paperloom_progress
        //   rejects longer strings anyway, but capping here avoids holding
        //   a multi-KB hostile string in heap.)
        if (out_progress.percentage < 0.0f) out_progress.percentage = 0.0f;
        if (out_progress.percentage > 1.0f) out_progress.percentage = 1.0f;
        if (out_progress.progress.length() > 128) {
            Serial.printf("[kosync_client] pull: truncating progress field "
                          "(server returned %u chars)\n",
                          static_cast<unsigned>(out_progress.progress.length()));
            out_progress.progress = out_progress.progress.substring(0, 128);
        }

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
    config.url                   = url.c_str();
    config.event_handler         = http_event_handler;
    config.user_data             = &buf;
    config.method                = HTTP_METHOD_PUT;
    config.timeout_ms            = kTimeoutMs;
    config.buffer_size           = kHttpBufSize;
    config.buffer_size_tx        = kHttpBufSize;
    // Cert pinning + no redirect — see pullProgress() for rationale.
    config.cert_pem              = PAPERLOOM_TRUSTED_ROOTS;
    config.disable_auto_redirect = true;

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

    if (buf.truncated) {
        Serial.printf("[kosync_client] push: response truncated (cap=%d)\n",
                      kResponseHardCap);
        out_error = kErrServer;
        esp_http_client_cleanup(client);
        return 500;
    }

    Serial.printf("[kosync_client] push: status=%d\n", status);

    if (status == 200 || status == 201) {
        // Some kosync forks and reverse-proxy frontends return 2xx with an
        // error payload (e.g. {"ok":false,"error":"..."}). Best-effort
        // inspect — if the body parses as JSON and signals failure, treat
        // as a server error instead of silently flagging the push ok.
        // A non-JSON or empty body is the upstream-kosync norm and stays
        // a success.
        if (buf.data && buf.len > 0 && !buf.truncated) {
            // 256-byte probe is deliberately small. Bodies between 257 B
            // and kResponseHardCap (4 KB) yield DeserializationError::NoMemory
            // and fall through to the success path — that is intentional:
            // upstream kosync push responses are empty or <50 bytes, so a
            // large body is almost certainly a non-JSON proxy page that we
            // don't want to scan. Widening this probe would increase stack
            // pressure during the TLS session — leave at 256.
            StaticJsonDocument<256> probe;
            DeserializationError jerr =
                deserializeJson(probe, buf.data, buf.len);
            if (!jerr) {
                // Only reject "error" if it carries a non-empty string —
                // some stock kosync forks include `"error": null` or
                // `"error": ""` on a 2xx success and they MUST not be
                // wrongly rejected.
                bool hasError = false;
                if (probe.containsKey("error")) {
                    if (probe["error"].is<const char*>()) {
                        const char* e = probe["error"].as<const char*>();
                        hasError = (e != nullptr && e[0] != '\0');
                    } else if (!probe["error"].isNull()) {
                        // Non-string, non-null "error" key (e.g. object,
                        // number) — treat as a real error signal.
                        hasError = true;
                    }
                }
                const bool okFalse = probe.containsKey("ok") &&
                                     probe["ok"].is<bool>() &&
                                     probe["ok"].as<bool>() == false;
                if (hasError || okFalse) {
                    Serial.printf("[kosync_client] push: 2xx with error "
                                  "payload — rejecting\n");
                    out_error = kErrServer;
                    esp_http_client_cleanup(client);
                    return 500;
                }
            }
        }
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

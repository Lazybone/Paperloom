// kosync_http_handlers.cpp — Web-UI endpoints for KoSync credentials (WP-6a).
//
// These endpoints back the "KoSync" card on the settings page:
//   GET  /api/kosync-settings  → returns server / user / deviceName
//                                (NEVER the stored MD5 key — defense-in-depth
//                                even though the key isn't a plaintext secret).
//   POST /api/kosync-settings  → accepts { server, user, password, deviceName? },
//                                validates, hashes the password to MD5 hex,
//                                persists Settings, returns { ok:true }.
//
// Security notes:
//   • The plaintext password buffer is zeroed via a volatile-pointer pattern
//     with a read-back BEFORE the HTTP response is sent. This is best-effort
//     scrubbing in the Arduino String world (which may also hold short-buffer
//     copies elsewhere on the heap) but it eliminates the obvious lingering
//     copy in our handler frame.
//   • The GET handler sources every value directly from settings_get() —
//     never echoes anything from the request body or URL params — so reflected
//     XSS is not possible through this endpoint.
//   • The POST handler logs only outcome class, never the password, the MD5
//     digest, or the full request body.

#include "kosync_http_handlers.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md5.h>
#include <stdint.h>
#include <stdio.h>

#include "settings.h"
#include "tls_certs.h"
#include "kosync_validation.h"
#include "kosync_hash.h"
#include "reader.h"

// Defined in main.cpp — the single live BookReader. Read-only access from
// the diagnostic /api/kosync-hash endpoint below.
extern BookReader reader;

namespace {

// ─── Static state ────────────────────────────────────────────────────────
//
// WebServer's .on() takes a function pointer; we capture the active server
// instance into a file-scope pointer so the registered handlers can reach
// .arg("plain") / .send() without a closure.
WebServer* g_server = nullptr;

// ─── Helpers ─────────────────────────────────────────────────────────────

// Lower-case the "https://" scheme prefix only; pass everything else through.
// Duplicates the helper of the same name in settings.cpp (which is static and
// therefore not importable). Kept tiny and behaviour-identical on purpose;
// if either copy ever changes, update both.
String normalize_https_scheme(const String& url) {
    if (url.length() < 8) return url;
    String head = url.substring(0, 8);
    head.toLowerCase();
    if (head == "https://") {
        String rest = url.substring(8);
        return head + rest;
    }
    return url;
}

// Matches ^[A-Za-z0-9_.-]{1,32}$ — shared with KosyncClient via
// kosync_validation.h so both validation sites stay in lock-step.
// Anonymous-namespace member already has internal linkage; no `inline`
// needed (the compiler inlines small one-liners anyway with -O2).
bool is_valid_kosync_user(const String& u) {
    return kosync_validation::is_valid_username(u);
}

// Compute MD5(plaintext) → 32-char lowercase hex string.
//
// mbedtls 3.x (ESP-IDF 5.x / Arduino-ESP32 3.x) removed the _ret suffix
// variants. Use the plain one-shot mbedtls_md5() with identical signature.
String md5_hex(const String& plaintext) {
    uint8_t digest[16];
    const int rc = mbedtls_md5(
        reinterpret_cast<const uint8_t*>(plaintext.c_str()),
        plaintext.length(),
        digest);
    if (rc != 0) {
        // Hardware-backed MD5 reported an error — return empty hex so the
        // caller can treat it as a save-failure rather than persist a
        // zero-byte key.
        return String();
    }

    char hex[33];
    for (int i = 0; i < 16; ++i) {
        snprintf(hex + (i * 2), 3, "%02x", digest[i]);
    }
    hex[32] = '\0';
    return String(hex);
}

// Best-effort plaintext scrub: walk the underlying char buffer through a
// volatile pointer (so the compiler can't optimise the writes away), force
// a read-back of byte 0, then drop the String entirely.
void scrub_plaintext(String& pw) {
    if (pw.length() > 0) {
        volatile char* vp = const_cast<volatile char*>(pw.c_str());
        for (size_t i = 0; i < pw.length(); ++i) vp[i] = 0;
        (void)vp[0];
    }
    pw = String();
}

void send_json(int code, const String& body) {
    if (g_server) g_server->send(code, "application/json", body);
}

void send_error(int code, const char* key, const char* reason) {
    StaticJsonDocument<128> doc;
    doc["ok"] = false;
    String err = String(key) + ": " + reason;
    doc["error"] = err;
    String out;
    serializeJson(doc, out);
    send_json(code, out);
}

// ─── Handlers ────────────────────────────────────────────────────────────

void handle_get() {
    // Source values directly from Settings — never from request input.
    Settings& s = settings_get();
    StaticJsonDocument<384> doc;
    doc["ok"]         = true;
    doc["server"]     = s.kosyncServer;
    doc["user"]       = s.kosyncUser;
    doc["deviceName"] = s.kosyncDeviceName;
    // NOTE: kosyncKey is intentionally never exposed.
    String body;
    serializeJson(doc, body);
    send_json(200, body);
}

void handle_post() {
    if (!g_server) return;

    String body = g_server->arg("plain");
    if (body.length() > 1024) {
        send_error(413, "body", "too large");
        return;
    }

    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body)) {
        send_error(400, "json", "invalid");
        return;
    }

    // Required fields.
    if (!doc.containsKey("server") ||
        !doc.containsKey("user")   ||
        !doc.containsKey("password")) {
        send_error(400, "field", "server/user/password required");
        return;
    }

    String server = doc["server"].as<String>();
    String user   = doc["user"].as<String>();
    String pw     = doc["password"].as<String>();
    String device = doc.containsKey("deviceName")
                        ? doc["deviceName"].as<String>()
                        : String();

    // Validation — server. Length 8..256, must begin with literal "https://"
    // (case-sensitive byte-compare AFTER scheme-normalization).
    if (server.length() < 8 || server.length() > 256) {
        scrub_plaintext(pw);
        send_error(400, "server", "length 8..256");
        return;
    }
    server = normalize_https_scheme(server);
    if (!server.startsWith("https://")) {
        scrub_plaintext(pw);
        send_error(400, "server", "must start with https://");
        return;
    }

    // Validation — user.
    if (!is_valid_kosync_user(user)) {
        scrub_plaintext(pw);
        send_error(400, "user", "1..32 chars [A-Za-z0-9_.-]");
        return;
    }

    // Validation — password length.
    if (pw.length() < 1 || pw.length() > 128) {
        scrub_plaintext(pw);
        send_error(400, "password", "length 1..128");
        return;
    }

    // Validation — optional deviceName.
    if (doc.containsKey("deviceName")) {
        if (device.length() < 1 || device.length() > 32) {
            scrub_plaintext(pw);
            send_error(400, "deviceName", "length 1..32");
            return;
        }
    }

    // Hash, THEN scrub the plaintext BEFORE responding.
    const String key = md5_hex(pw);
    scrub_plaintext(pw);
    if (key.length() != 32) {
        // md5_ret() reported failure; refuse to persist a partial credential.
        Serial.printf("[kosync_http] settings_post: md5_failed\n");
        send_error(500, "hash", "MD5 unavailable");
        return;
    }

    // Persist.
    Settings& s = settings_get();
    s.kosyncServer = server;
    s.kosyncUser   = user;
    s.kosyncKey    = key;
    if (doc.containsKey("deviceName")) {
        s.kosyncDeviceName = device;
    }
    // Clear the runtime invalid flag; we just stored values that passed all
    // boundary checks here, so downstream readers should treat them as valid.
    s.kosyncCredentialsInvalid = false;

    if (!settings_save()) {
        Serial.printf("[kosync_http] settings_post: save_failed\n");
        send_error(500, "save", "SD write failed");
        return;
    }

    Serial.printf("[kosync_http] settings_post: ok\n");
    send_json(200, String("{\"ok\":true}"));
}

// POST /api/kosync-register — proxy a kosync account-creation request to the
// device's configured kosync server. The request body MUST NOT contain a
// `server` field; the target URL is always sourced from Settings.kosyncServer
// to prevent credential exfiltration via a forged target.
//
// On success the device returns { ok:true }. On the well-known kosync 402
// (username already taken) the device translates to 409 + { error:
// "username_taken" } so the UI can surface the conflict clearly without
// leaking upstream protocol details.
void handle_post_register() {
    if (!g_server) return;

    if (!g_server->hasArg("plain")) {
        send_json(400, String("{\"ok\":false,\"error\":\"missing body\"}"));
        return;
    }

    String body = g_server->arg("plain");
    if (body.length() > 1024) {
        send_error(413, "body", "too large");
        return;
    }

    DynamicJsonDocument doc(512);
    const DeserializationError de = deserializeJson(doc, body);
    if (de) {
        send_json(400, String("{\"ok\":false,\"error\":\"invalid json\"}"));
        return;
    }

    // C1: reject any client-supplied `server` to prevent credential
    // exfiltration. The target URL is always Settings.kosyncServer.
    if (doc.containsKey("server")) {
        send_json(400, String("{\"ok\":false,\"error\":\"unknown_field: server\"}"));
        return;
    }

    String user = String(doc["user"] | "");
    String pw   = String(doc["password"] | "");

    if (!is_valid_kosync_user(user)) {
        scrub_plaintext(pw);
        send_json(400, String("{\"ok\":false,\"error\":\"user: invalid charset or length\"}"));
        return;
    }
    if (pw.length() < 1 || pw.length() > 128) {
        scrub_plaintext(pw);
        send_json(400, String("{\"ok\":false,\"error\":\"password: length 1..128\"}"));
        return;
    }

    Settings& s = settings_get();
    String target = s.kosyncServer;
    if (target.length() < 8 || !target.startsWith("https://")) {
        scrub_plaintext(pw);
        send_json(400, String("{\"ok\":false,\"error\":\"kosyncServer not configured\"}"));
        return;
    }

    // Hash plaintext IMMEDIATELY, then zero plaintext before any network I/O.
    const String pwMd5 = md5_hex(pw);
    scrub_plaintext(pw);
    if (pwMd5.length() != 32) {
        Serial.printf("[kosync_http] register_post: md5_failed\n");
        send_json(500, String("{\"ok\":false,\"error\":\"md5 failed\"}"));
        return;
    }

    // Build outgoing JSON: { username, password: <md5-hex> }.
    StaticJsonDocument<256> outDoc;
    outDoc["username"] = user;
    outDoc["password"] = pwMd5;
    String outBody;
    serializeJson(outDoc, outBody);

    // POST to {target}/users/create with the project-wide pinned CA bundle.
    WiFiClientSecure tls;
    tls.setCACert(PAPERLOOM_TRUSTED_ROOTS);
    tls.setHandshakeTimeout(15);  // seconds

    HTTPClient http;
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

    const String url = target + "/users/create";
    if (!http.begin(tls, url)) {
        send_json(502, String("{\"ok\":false,\"error\":\"begin failed\"}"));
        Serial.printf("[kosync_http] register_post: upstream=begin_failed\n");
        return;
    }
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST(outBody);
    http.end();

    if (code == 200 || code == 201) {
        send_json(200, String("{\"ok\":true}"));
    } else if (code == 402) {
        // kosync convention: 402 Payment Required = username taken.
        send_json(409, String("{\"ok\":false,\"error\":\"username_taken\"}"));
    } else if (code == 401 || code == 403) {
        send_json(401, String("{\"ok\":false,\"error\":\"auth\"}"));
    } else if (code <= 0) {
        send_json(502, String("{\"ok\":false,\"error\":\"server_unreachable\"}"));
    } else {
        send_json(502, String("{\"ok\":false,\"error\":\"server: ") + String(code) + "\"}");
    }

    Serial.printf("[kosync_http] register_post: upstream=%d response=%s\n", code,
                  (code == 200 || code == 201) ? "ok" : "fail");
}

// Diagnostic endpoint — returns the KoSync document hash for either the
// currently-open book (no path arg) or an arbitrary EPUB on the SD card
// (?path=/books/foo.epub). Useful when the conflict dialog shows
// nonsense remote data: compare the hash here against what's stored on
// the kosync server for the same hash to confirm the device is asking
// about the book it thinks it's asking about.
//
// No secrets, no PII — hash is a 32-char KOReader-canonical partial-MD5
// over up to 12 × 1 KiB chunks at non-contiguous offsets (see
// kosync_hash.cpp). Title and filepath are already user-visible elsewhere.
//
// Concurrency: this handler runs on the same Arduino loop task as the
// kosync sync coordinator (no xTaskCreate in the codebase), so the
// non-reentrant kosync_compute_document_hash() is safe here. WiFi-upload
// mode and an active kosync sync are also mutually exclusive at the UI
// state-machine level (STATE_WIFI vs STATE_SYNC_PROGRESS), so the
// diagnostic endpoint cannot be hit while runHashing() is in flight.
void handle_get_hash() {
    String title;
    String filepath;
    String hash;

    if (g_server && g_server->hasArg("path")) {
        // Hash a specific file (file-browser UI use case). Reject paths
        // that don't look like an EPUB on the SD card root — defensive
        // against `?path=/etc/passwd` style probing even though the SD
        // filesystem doesn't expose host paths. Also block dot-prefixed
        // segments (e.g. `/.progress`, `/.settings.json`) so the
        // diagnostic endpoint cannot be used to fingerprint internal
        // state files via their partial-MD5.
        filepath = g_server->arg("path");
        if (filepath.length() == 0 || filepath.charAt(0) != '/' ||
            filepath.indexOf("..") >= 0 ||
            filepath.indexOf("/.") >= 0) {
            send_json(400, "{\"ok\":false,\"error\":\"bad path\"}");
            return;
        }
        hash  = kosync_compute_document_hash(filepath);
        // No title lookup for arbitrary files — the UI already shows
        // the filename next to the hash, and parsing the EPUB just for
        // a title here would double-read the file.
        title = "";
    } else {
        // Default: currently-open book.
        title    = reader.getTitle();
        filepath = reader.getFilepath();
        hash     = reader.getDocumentHash();
    }

    // JSON-escape: titles can contain ", \ and control chars. Cheap escape.
    auto esc = [](const String& s) -> String {
        String o;
        o.reserve(s.length() + 8);
        for (size_t i = 0; i < s.length(); ++i) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n";  break;
                case '\r': o += "\\r";  break;
                case '\t': o += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        // Cast back to unsigned char before formatting:
                        // on platforms where `char` is signed (most ARM/
                        // Xtensa toolchains), passing a negative char to
                        // `%04x` sign-extends to int and emits a wrong
                        // multi-byte hex escape. The earlier cast in the
                        // comparison didn't propagate to the format call.
                        char b[8];
                        snprintf(b, sizeof(b), "\\u%04x",
                                 static_cast<unsigned char>(c));
                        o += b;
                    } else {
                        o += c;
                    }
            }
        }
        return o;
    };

    String body;
    body.reserve(256 + title.length() + filepath.length());
    body += "{\"ok\":true,\"hash\":\"";
    body += esc(hash);
    body += "\",\"title\":\"";
    body += esc(title);
    body += "\",\"filepath\":\"";
    body += esc(filepath);
    body += "\",\"hashLen\":";
    body += String(hash.length());
    body += "}";
    send_json(200, body);
}

}  // namespace

void kosync_http_register_handlers(WebServer& server) {
    g_server = &server;
    server.on("/api/kosync-settings", HTTP_GET,  handle_get);
    server.on("/api/kosync-settings", HTTP_POST, handle_post);
    server.on("/api/kosync-register", HTTP_POST, handle_post_register);
    server.on("/api/kosync-hash",     HTTP_GET,  handle_get_hash);
}

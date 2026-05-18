#include "serial_console.h"
#include "settings.h"
#include "config.h"
#include "kosync_sync.h"

#include <Arduino.h>
#include <WiFi.h>
#include <MD5Builder.h>

// Maximum line length we accept before declaring garbage input.
static const size_t MAX_LINE_LEN = 512;

static String _lineBuf;
static bool   _overflow = false;

// ── Helpers ──────────────────────────────────────────────────────────────────

static void sc_ok(const String& msg = "") {
    if (msg.length() > 0) {
        Serial.print("OK ");
        Serial.println(msg);
    } else {
        Serial.println("OK");
    }
}

static void sc_err(const String& msg) {
    Serial.print("ERR ");
    Serial.println(msg);
}

// ── Command handlers ──────────────────────────────────────────────────────────

static void cmd_help() {
    Serial.println("OK_BEGIN");
    Serial.println("HELP");
    Serial.println("STATUS");
    Serial.println("WIFI_SET\t<ssid>\t<password>");
    Serial.println("WIFI_SCAN");
    Serial.println("KOSYNC_SET\t<server>\t<user>\t<password>\t<device>");
    Serial.println("REBOOT");
    Serial.println("OK_END");
}

static void cmd_status() {
    const Settings& s = settings_get();
    Serial.println("OK_BEGIN");
    Serial.print("SSID\t");       Serial.println(s.wifiSSID);
    Serial.print("IP\t");         Serial.println(WiFi.localIP().toString());
    Serial.print("WIFI_CONNECTED\t");
    Serial.println(WiFi.status() == WL_CONNECTED ? "1" : "0");
    Serial.print("KOSYNC_USER\t");   Serial.println(s.kosyncUser);
    Serial.print("KOSYNC_SERVER\t"); Serial.println(s.kosyncServer);
    Serial.print("KOSYNC_DEVICE\t"); Serial.println(s.kosyncDeviceName);
    Serial.print("FREE_HEAP\t");     Serial.println(ESP.getFreeHeap());
    Serial.print("DMA_LARGEST\t");   Serial.println(heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    Serial.print("FW_VERSION\t");    Serial.println(FIRMWARE_VERSION);
    Serial.println("OK_END");
}

static void cmd_wifi_set(const String& ssid, const String& pass) {
    if (ssid.length() == 0) {
        sc_err("SSID must not be empty");
        return;
    }
    if (!settings_set_wifi(ssid, pass)) {
        sc_err("settings_save failed");
        return;
    }
    sc_ok();
}

static void cmd_wifi_scan() {
    if (kosync_is_coordinator_initialized() && kosync_get_coordinator().isBusy()) {
        sc_err("Sync läuft — bitte später");
        return;
    }
    Serial.println("OK_BEGIN");
    int n = WiFi.scanNetworks(false);  // synchronous, ~2-3 s
    if (n < 0) {
        // scanNetworks returns WIFI_SCAN_FAILED (-2) or similar on error
        Serial.println("OK_END");
        return;
    }
    for (int i = 0; i < n; ++i) {
        Serial.print("NET\t");
        Serial.print(WiFi.RSSI(i));
        Serial.print("\t");
        Serial.print(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "1" : "0");
        Serial.print("\t");
        Serial.println(WiFi.SSID(i));
    }
    WiFi.scanDelete();
    Serial.println("OK_END");
}

static void cmd_kosync_set(const String& server, const String& user,
                           const String& password, const String& device) {
    if (server.length() == 0) {
        sc_err("server must not be empty");
        return;
    }
    if (!server.startsWith("https://") && !server.startsWith("HTTPS://")) {
        sc_err("server must start with https://");
        return;
    }
    if (user.length() == 0) {
        sc_err("user must not be empty");
        return;
    }

    // Hash the plaintext password with MD5 (same convention used in WP-3/WP-5/WP-6).
    MD5Builder md5;
    md5.begin();
    md5.add(password);
    md5.calculate();
    String md5key = md5.toString();  // 32-char lowercase hex

    if (!settings_set_kosync(server, user, md5key, device)) {
        sc_err("settings_save failed");
        return;
    }
    sc_ok();
}

static void cmd_reboot() {
    Serial.println("OK reboot");
    Serial.flush();
    delay(100);
    ESP.restart();
}

// ── Line parser ───────────────────────────────────────────────────────────────

static void parse_and_execute(const String& line) {
    if (line.length() == 0) return;

    // Split on TAB into up to 6 tokens (verb + 5 args max).
    static const int MAX_TOKENS = 6;
    String tokens[MAX_TOKENS];
    int ntok = 0;
    int start = 0;
    while (ntok < MAX_TOKENS) {
        int pos = line.indexOf('\t', start);
        if (pos < 0) {
            tokens[ntok++] = line.substring(start);
            break;
        }
        tokens[ntok++] = line.substring(start, pos);
        start = pos + 1;
    }

    if (ntok == 0) return;

    String verb = tokens[0];
    verb.toUpperCase();

    if (verb == "HELP") {
        cmd_help();
    } else if (verb == "STATUS") {
        cmd_status();
    } else if (verb == "WIFI_SET") {
        if (ntok < 3) { sc_err("usage: WIFI_SET<TAB><ssid><TAB><password>"); return; }
        cmd_wifi_set(tokens[1], tokens[2]);
    } else if (verb == "WIFI_SCAN") {
        cmd_wifi_scan();
    } else if (verb == "KOSYNC_SET") {
        if (ntok < 4) { sc_err("usage: KOSYNC_SET<TAB><server><TAB><user><TAB><password>[<TAB><device>]"); return; }
        // device is optional (index 4); empty string keeps the existing default
        String device = (ntok >= 5) ? tokens[4] : String();
        cmd_kosync_set(tokens[1], tokens[2], tokens[3], device);
    } else if (verb == "REBOOT") {
        cmd_reboot();
    } else {
        sc_err("unknown command — send HELP for a list");
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void serial_console_tick() {
    while (Serial.available() > 0) {
        char c = (char)Serial.read();

        if (c == '\r') continue;  // silently drop CR

        if (c == '\n') {
            if (_overflow) {
                sc_err("line too long (max 512 bytes) — command ignored");
                _overflow = false;
            } else {
                parse_and_execute(_lineBuf);
            }
            _lineBuf = "";
            continue;
        }

        if (_overflow) continue;  // discard until next newline

        _lineBuf += c;
        if (_lineBuf.length() >= MAX_LINE_LEN) {
            _overflow = true;
        }
    }
}

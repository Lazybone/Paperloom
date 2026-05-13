#include "settings.h"
#include "config.h"
#include "storage_utils.h"
#include "button_action.h"
#include <SD.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>  // WP-3: WiFi.macAddress() for default kosync device name

static Settings _settings;
static const char* SETTINGS_PATH     = "/.settings.json";
static const char* SETTINGS_TMP      = "/.settings.tmp";
// Legacy path before the move to SD root.  Read once and migrated to the
// new location on the next save.
static const char* SETTINGS_PATH_OLD = "/books/.settings.json";

// NVS namespace + key for the settings-mirror. Namespace is distinct from
// the "ereader" namespace (used for sleep/reader state) so a NVS erase
// scoped to one of them does not nuke the other. Namespace fits the
// 15-char NVS limit.
static const char* NVS_NS  = "ereader_set";
static const char* NVS_KEY = "json";

// WP-3: individual NVS mirror keys for kosync fields. Stored alongside
// the JSON blob so the credentials survive a corrupted JSON mirror and
// can be inspected/cleared independently. All keys are ≤15 chars to
// honour the NVS limit.
static const char* NVS_KEY_KOSYNC_SRV = "kosyncSrv";
static const char* NVS_KEY_KOSYNC_USR = "kosyncUsr";
static const char* NVS_KEY_KOSYNC_KEY = "kosyncKey";
static const char* NVS_KEY_KOSYNC_DEV = "kosyncDev";

static const char* KOSYNC_DEFAULT_SERVER = "https://kosync.eu";

// Bump on any breaking schema change (renamed field, removed default,
// changed semantic of an existing field). Read by settings_init() so a
// future load can branch on it.
//   v3: replaced bool serifFont with uint8_t fontFamily (0=Sans/Lexend,
//       1=Serif/Literata, 2=Slab/Bitter).  Old bool migrates 1:1
//       (true → Serif).
static const int SETTINGS_SCHEMA_VERSION = 3;

// =========================================================================
// WP-3: KoSync field helpers
// =========================================================================

// Lower-case the "https://" scheme prefix only; host + path preserve their
// original case (some kosync deployments use case-sensitive path routing).
// Anything that doesn't begin with a recognised https scheme is passed
// through unchanged — downstream byte-compare against "https://" will
// reject it.
static String normalize_https_scheme(const String& url) {
    if (url.length() < 8) return url;
    String head = url.substring(0, 8);
    head.toLowerCase();
    if (head == "https://") {
        String rest = url.substring(8);
        return head + rest;  // lower only the scheme; preserve case of host+path
    }
    return url;  // pass through; downstream byte-compare rejects non-https
}

// Returns true when `user` matches ^[A-Za-z0-9_.-]{1,32}$.
// Empty is rejected here; the caller decides whether empty is OK.
static bool is_valid_kosync_user(const String& user) {
    const size_t n = user.length();
    if (n == 0 || n > 32) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = user.charAt(i);
        const bool ok = (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// Returns true when `key` is exactly 32 lowercase hex characters.
// Empty is rejected here; the caller decides whether empty is OK.
static bool is_valid_kosync_key(const String& key) {
    if (key.length() != 32) return false;
    for (size_t i = 0; i < 32; ++i) {
        const char c = key.charAt(i);
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

// "paperloom-" + last 4 hex chars of the WiFi MAC (lowercase, no colons).
// WiFi need not be connected — macAddress() returns the station MAC once
// the radio has been brought up at least once (esp_wifi_init). If the MAC
// is unavailable (e.g. very early boot) we fall back to a stable sentinel.
static String compute_default_device_name() {
    String mac = WiFi.macAddress();           // "AA:BB:CC:DD:EE:FF" or ""
    mac.replace(":", "");
    mac.toLowerCase();
    if (mac.length() < 4) {
        return String("paperloom-0000");      // pre-WiFi-init fallback
    }
    return String("paperloom-") + mac.substring(mac.length() - 4);
}

void settings_set_default() {
    _settings.fontSize        = 2;  // legacy: maps to fontSizeLevel 2 (M)
    _settings.fontSizeLevel   = 2;  // M (default)
    _settings.lineSpacingLevel = 2;
    _settings.fontFamily      = 0;  // Sans (Lexend Deca)
    _settings.sleepTimeoutMin = 5;
    _settings.refreshEveryPages = 4;
    _settings.wifiSSID        = WIFI_SSID;
    _settings.wifiPass        = WIFI_PASS;
    _settings.showPageNumbers = true;
    _settings.showBattery     = true;
    _settings.tapZoneLayout   = 0;
    _settings.libraryViewMode = 0;
    _settings.librarySortOrder = 0;
    _settings.posterShowCovers = false;
    _settings.frontlightEnabled    = false;
    _settings.frontlightBrightness = 30;
    // User button is opt-in: stays disabled until user toggles it in
    // settings. Avoids spurious actions on hardware without GPIO 48 wired.
    _settings.userButtonEnabled      = false;
    _settings.userButtonTapAction    = 1;  // sensible default once enabled
    _settings.userButtonDoubleAction = 2;
    _settings.userButtonLongAction   = 3;
    // Boot button defaults preserve historical hardcoded behaviour
    // (next/prev/sleep) so an OTA update doesn't change the feel of the
    // device for existing users.
    _settings.bootButtonEnabled      = true;
    _settings.bootButtonTapAction    = BTN_ACTION_NEXT_PAGE;
    _settings.bootButtonDoubleAction = BTN_ACTION_PREV_PAGE;
    _settings.bootButtonLongAction   = BTN_ACTION_SLEEP;

    // WP-3: KoSync defaults. Server seeded only when the field is *missing*
    // from storage (handled at load time); a deliberately-cleared empty
    // string survives a defaults-reset because the load path overrides it.
    _settings.kosyncServer             = KOSYNC_DEFAULT_SERVER;
    _settings.kosyncUser               = "";
    _settings.kosyncKey                = "";
    _settings.kosyncDeviceName         = "";  // resolved lazily on first load
    _settings.kosyncCredentialsInvalid = false;
}

// Read the whole settings file from SD into a String. Returns empty
// String on missing-file / IO failure. Separated from parsing so the
// NVS fallback path can reuse the same apply logic.
static String read_text_file(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return String();
    String out;
    out.reserve((size_t)f.size() + 1);
    while (f.available()) {
        out += (char)f.read();
    }
    f.close();
    return out;
}

static String load_settings_text_from_sd(bool& fromLegacy) {
    fromLegacy = false;
    String out = read_text_file(SETTINGS_PATH);
    if (out.length() > 0) return out;
    // Fall back to the legacy /books location so users upgrading from an
    // older firmware don't lose their settings on first boot.
    out = read_text_file(SETTINGS_PATH_OLD);
    if (out.length() > 0) {
        fromLegacy = true;
        Serial.println("Settings: found legacy /books/.settings.json — migrating to SD root");
    }
    return out;
}

static bool load_settings_text_from_nvs(String& out) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return false;
    if (!prefs.isKey(NVS_KEY)) {
        prefs.end();
        return false;
    }
    out = prefs.getString(NVS_KEY, "");
    prefs.end();
    return out.length() > 0;
}

static bool save_settings_text_to_nvs(const String& json) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return false;
    size_t written = prefs.putString(NVS_KEY, json);
    prefs.end();
    return written > 0;
}

// WP-3: backfill kosync fields from the individual NVS mirror keys.
// Called after the primary JSON load chain (SD/NVS-blob) so the
// per-field NVS entries act as a tertiary recovery for credentials —
// useful when the SD file is missing AND the NVS JSON blob got
// truncated/corrupted but the individual keys are intact. Only
// overwrites the in-memory value when an NVS key is *present*; the
// "deliberately-cleared empty string" semantics from JSON load are
// preserved by reading isKey() before getString().
static void load_kosync_overrides_from_nvs() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return;
    if (prefs.isKey(NVS_KEY_KOSYNC_SRV)) {
        String srv = prefs.getString(NVS_KEY_KOSYNC_SRV, "");
        if (srv.length() <= 256) {
            _settings.kosyncServer = normalize_https_scheme(srv);
        }
    }
    if (prefs.isKey(NVS_KEY_KOSYNC_USR)) {
        String usr = prefs.getString(NVS_KEY_KOSYNC_USR, "");
        if (usr.length() == 0) {
            _settings.kosyncUser = "";
        } else if (is_valid_kosync_user(usr)) {
            _settings.kosyncUser = usr;
        } else {
            Serial.printf("[settings] kosyncUser invalid format → reset\n");
            _settings.kosyncUser = "";
            _settings.kosyncCredentialsInvalid = true;
        }
    }
    if (prefs.isKey(NVS_KEY_KOSYNC_KEY)) {
        String key = prefs.getString(NVS_KEY_KOSYNC_KEY, "");
        if (key.length() == 0) {
            _settings.kosyncKey = "";
        } else if (is_valid_kosync_key(key)) {
            _settings.kosyncKey = key;
        } else {
            Serial.printf("[settings] kosyncKey invalid format → reset\n");
            _settings.kosyncKey = "";
            _settings.kosyncCredentialsInvalid = true;
        }
    }
    if (prefs.isKey(NVS_KEY_KOSYNC_DEV)) {
        String dev = prefs.getString(NVS_KEY_KOSYNC_DEV, "");
        if (dev.length() > 32) dev = dev.substring(0, 32);
        if (dev.length() > 0) _settings.kosyncDeviceName = dev;
    }
    prefs.end();
}

// Parse a JSON blob and apply it to _settings.  Returns true on success.
// On schema-too-new or parse-error returns false and leaves _settings as
// the caller arranged (typically the defaults from settings_set_default()).
static bool apply_settings_json(const String& json, const char* sourceLabel) {
    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("Settings: parse error from %s (%s)\n",
                      sourceLabel, err.c_str());
        return false;
    }

    // If the file is from a NEWER firmware than we know about, the new
    // fields would silently fall back to defaults (data loss on rollback).
    // Refuse to apply so the caller can try the next source instead.
    int loadedSchema = doc["schemaVersion"] | 1;
    if (loadedSchema > SETTINGS_SCHEMA_VERSION) {
        Serial.printf("Settings: %s schema %d > current %d — ignoring\n",
                      sourceLabel, loadedSchema, SETTINGS_SCHEMA_VERSION);
        return false;
    }
    if (loadedSchema != SETTINGS_SCHEMA_VERSION) {
        Serial.printf("Settings: %s schema=%d → %d, applying field-level migrations\n",
                      sourceLabel, loadedSchema, SETTINGS_SCHEMA_VERSION);
    }

    _settings.fontSize        = doc["fontSize"]        | 2;
    _settings.fontSizeLevel   = doc["fontSizeLevel"]   | -1;
    _settings.lineSpacingLevel = doc["lineSpacingLevel"] | 2;
    // fontFamily: prefer the new key.  Validate the JSON int as a signed
    // int before narrowing to uint8_t so a garbage value (e.g. 300) is
    // rejected at the boundary rather than silently truncated by the cast.
    // When the new key is missing, fall back to the pre-v3 bool "serifFont"
    // (true → Serif=1, false → Sans=0) and log so a maintainer can tell
    // the migration ran.
    if (doc.containsKey("fontFamily")) {
        int v = doc["fontFamily"].as<int>();
        if (v >= 0 && v < FONT_FAMILY_COUNT) {
            _settings.fontFamily = (uint8_t)v;
        } else {
            Serial.printf("Settings: fontFamily=%d out of range [0,%d), defaulting to 0\n",
                          v, FONT_FAMILY_COUNT);
            _settings.fontFamily = 0;
        }
    } else {
        _settings.fontFamily = (doc["serifFont"] | false) ? 1 : 0;
        Serial.printf("Settings: migrated legacy serifFont → fontFamily=%u\n",
                      (unsigned)_settings.fontFamily);
    }
    _settings.sleepTimeoutMin = doc["sleepTimeoutMin"]  | 5;
    _settings.refreshEveryPages = doc["refreshEveryPages"] | 4;
    _settings.wifiSSID        = doc["wifiSSID"]         | WIFI_SSID;
    _settings.wifiPass        = doc["wifiPass"]         | WIFI_PASS;
    _settings.showPageNumbers = doc["showPageNumbers"]  | true;
    _settings.showBattery     = doc["showBattery"]      | true;
    _settings.tapZoneLayout   = doc["tapZoneLayout"]    | 0;
    _settings.libraryViewMode = doc["libraryViewMode"]  | 0;
    _settings.librarySortOrder = doc["librarySortOrder"] | 0;
    _settings.posterShowCovers = doc["posterShowCovers"] | false;
    _settings.frontlightEnabled    = doc["frontlightEnabled"]    | false;
    _settings.frontlightBrightness = doc["frontlightBrightness"] | 30;
    if (_settings.frontlightBrightness > 100) _settings.frontlightBrightness = 100;
    _settings.userButtonEnabled      = doc["userButtonEnabled"]      | false;
    _settings.userButtonTapAction    = doc["userButtonTapAction"]    | 1;
    _settings.userButtonDoubleAction = doc["userButtonDoubleAction"] | 2;
    _settings.userButtonLongAction   = doc["userButtonLongAction"]   | 3;
    _settings.bootButtonEnabled      = doc["bootButtonEnabled"]      | true;
    _settings.bootButtonTapAction    = doc["bootButtonTapAction"]    | (uint8_t)BTN_ACTION_NEXT_PAGE;
    _settings.bootButtonDoubleAction = doc["bootButtonDoubleAction"] | (uint8_t)BTN_ACTION_PREV_PAGE;
    _settings.bootButtonLongAction   = doc["bootButtonLongAction"]   | (uint8_t)BTN_ACTION_SLEEP;

    // WP-3: KoSync fields. Distinguish "key missing entirely" (→ seed
    // default server) from "key present but empty string" (→ user cleared
    // it deliberately, leave empty). Validation is destructive: a stored
    // value that fails its rules is reset to empty AND raises the
    // runtime kosyncCredentialsInvalid flag for downstream UI.
    _settings.kosyncCredentialsInvalid = false;

    if (doc.containsKey("kosyncServer")) {
        String srv = doc["kosyncServer"].as<const char*>() ? String(doc["kosyncServer"].as<const char*>()) : String();
        if (srv.length() > 256) srv = "";          // length cap — reject silently
        srv = normalize_https_scheme(srv);
        // WP-3 fix (iter-1 SEC-LOW): also enforce https:// on load. A manually-
        // edited settings file could carry an http:// URL; without this guard,
        // the value would only fail downstream in KosyncClient with a generic
        // "Server nicht erreichbar" toast.
        if (srv.length() > 0 && !srv.startsWith("https://")) {
            Serial.printf("[settings] kosyncServer not https:// → reset\n");
            srv = "";
            _settings.kosyncCredentialsInvalid = true;
        }
        _settings.kosyncServer = srv;              // empty here = deliberate clear
    } else {
        _settings.kosyncServer = KOSYNC_DEFAULT_SERVER;
    }

    if (doc.containsKey("kosyncUser")) {
        String usr = doc["kosyncUser"].as<const char*>() ? String(doc["kosyncUser"].as<const char*>()) : String();
        if (usr.length() == 0) {
            _settings.kosyncUser = "";
        } else if (is_valid_kosync_user(usr)) {
            _settings.kosyncUser = usr;
        } else {
            Serial.printf("[settings] kosyncUser invalid format → reset\n");
            _settings.kosyncUser = "";
            _settings.kosyncCredentialsInvalid = true;
        }
    } else {
        _settings.kosyncUser = "";
    }

    if (doc.containsKey("kosyncKey")) {
        String key = doc["kosyncKey"].as<const char*>() ? String(doc["kosyncKey"].as<const char*>()) : String();
        if (key.length() == 0) {
            _settings.kosyncKey = "";
        } else if (is_valid_kosync_key(key)) {
            _settings.kosyncKey = key;
        } else {
            Serial.printf("[settings] kosyncKey invalid format → reset\n");
            _settings.kosyncKey = "";
            _settings.kosyncCredentialsInvalid = true;
        }
    } else {
        _settings.kosyncKey = "";
    }

    if (doc.containsKey("kosyncDeviceName")) {
        String dev = doc["kosyncDeviceName"].as<const char*>() ? String(doc["kosyncDeviceName"].as<const char*>()) : String();
        if (dev.length() > 32) dev = "";           // length cap — reset, will be re-derived below
        _settings.kosyncDeviceName = dev;
    } else {
        _settings.kosyncDeviceName = "";
    }
    // Empty device name resolves lazily to "paperloom-<last4-MAC>"; the
    // computed value is written back to storage on the next settings_save().
    if (_settings.kosyncDeviceName.length() == 0) {
        _settings.kosyncDeviceName = compute_default_device_name();
    }

    // Migrate old fontSize (0-2) → fontSizeLevel (0-6) if fontSizeLevel wasn't saved
    if (_settings.fontSizeLevel < 0) {
        // Map old 0=small→1(S), 1=medium→2(M), 2=large→4(L)
        const int migration[] = {1, 2, 4};
        int old = _settings.fontSize;
        if (old < 0 || old > 2) old = 1;
        _settings.fontSizeLevel = migration[old];
    }

    // Clamp values — fontSizeLevel is a direct index into the FONT_LINE_SPACINGS /
    // FONT_MARGIN_X_VALUES arrays which have FONT_SIZE_LEVEL_COUNT entries; an
    // out-of-bound value causes UB on the next paginate.
    if (_settings.fontSizeLevel < 0 || _settings.fontSizeLevel >= FONT_SIZE_LEVEL_COUNT) {
        _settings.fontSizeLevel = 2;
    }
    if ((int)_settings.lineSpacingLevel >= LINE_SPACING_LEVEL_COUNT) _settings.lineSpacingLevel = 2;
    // Defence in depth: the JSON-level check above already covers the
    // happy path, but a future schema migration that bypasses that branch
    // would still need a final clamp before fontFamily indexes anything.
    if (_settings.fontFamily >= FONT_FAMILY_COUNT) {
        Serial.printf("Settings: fontFamily=%u clamped to 0 (post-load defence)\n",
                      (unsigned)_settings.fontFamily);
        _settings.fontFamily = 0;
    }
    if (_settings.frontlightBrightness > 100) _settings.frontlightBrightness = 100;
    if (_settings.sleepTimeoutMin < 1) _settings.sleepTimeoutMin = 5;
    if (_settings.refreshEveryPages < 1) _settings.refreshEveryPages = 4;
    if (_settings.libraryViewMode < 0 || _settings.libraryViewMode > 1) _settings.libraryViewMode = 0;
    if (_settings.librarySortOrder > 3) _settings.librarySortOrder = 0;
    // Button action enum is stored as uint8_t — JSON could carry any value;
    // the switch in button_action_execute() falls through to a silent no-op
    // for out-of-range values, but better to clamp at load.
    if (_settings.userButtonTapAction    >= BTN_ACTION_COUNT) _settings.userButtonTapAction    = 0;
    if (_settings.userButtonDoubleAction >= BTN_ACTION_COUNT) _settings.userButtonDoubleAction = 0;
    if (_settings.userButtonLongAction   >= BTN_ACTION_COUNT) _settings.userButtonLongAction   = 0;
    if (_settings.bootButtonTapAction    >= BTN_ACTION_COUNT) _settings.bootButtonTapAction    = 0;
    if (_settings.bootButtonDoubleAction >= BTN_ACTION_COUNT) _settings.bootButtonDoubleAction = 0;
    if (_settings.bootButtonLongAction   >= BTN_ACTION_COUNT) _settings.bootButtonLongAction   = 0;

    // Don't log the SSID — even though it's not a credential, it identifies
    // the user's home network on the serial port. Length-only is enough to
    // confirm the load worked.
    Serial.printf("Settings: loaded from %s (fontLevel=%d, family=%u, sleep=%dmin, refresh=%d, wifi_len=%u)\n",
                  sourceLabel,
                  _settings.fontSizeLevel, (unsigned)_settings.fontFamily,
                  _settings.sleepTimeoutMin,
                  _settings.refreshEveryPages,
                  (unsigned)_settings.wifiSSID.length());
    return true;
}

void settings_init() {
    settings_set_default();

    // Try SD first.  This is the user-visible storage and surviving across
    // a firmware re-flash is the whole point.
    bool fromLegacy = false;
    String sdJson = load_settings_text_from_sd(fromLegacy);
    if (sdJson.length() > 0 && apply_settings_json(sdJson, fromLegacy ? "SD-legacy" : "SD")) {
        if (fromLegacy) {
            // Rewrite at the new SD-root path (and NVS mirror) then drop
            // the legacy file so it doesn't shadow future updates.
            if (settings_save() && SD.exists(SETTINGS_PATH_OLD)) {
                SD.remove(SETTINGS_PATH_OLD);
                Serial.println("Settings: removed legacy /books/.settings.json");
            }
        } else {
            // Mirror current JSON into NVS so future boots have a fallback.
            save_settings_text_to_nvs(sdJson);
        }
        return;
    }

    // SD missing / corrupt / schema-too-new → try NVS mirror.  This catches
    // the case where the SD file was deleted, the FS got remounted on a
    // blank card, or a future-firmware schema poisoned the SD file.  NVS
    // survives a `pio run -t upload` (only `--target erase` wipes it).
    String nvsJson;
    if (load_settings_text_from_nvs(nvsJson) && apply_settings_json(nvsJson, "NVS")) {
        Serial.println("Settings: recovered from NVS — restoring SD file");
        settings_save();  // rewrites SD with valid JSON
        return;
    }

    // Both JSON sources unusable.  Defaults already applied — before
    // persisting them, try to recover kosync credentials from their
    // individual NVS mirror keys (WP-3 tertiary fallback). This is the
    // one piece of state where a re-entry burdens the user, so it's worth
    // a dedicated recovery path.
    load_kosync_overrides_from_nvs();

    Serial.println("Settings: no usable source, writing defaults");
    if (!settings_save()) Serial.println("Settings: initial save failed");
}

bool settings_save() {
    // Pool sized for ~30 fields + their string values (including WP-3
    // kosync fields whose server URL alone can be 256 chars); bump if you
    // add more keys — the overflowed() check below catches the silent-
    // truncate case.
    StaticJsonDocument<2048> doc;
    doc["schemaVersion"]   = SETTINGS_SCHEMA_VERSION;
    doc["fontSize"]        = _settings.fontSizeLevel; // write new level as fontSize too for compat
    doc["fontSizeLevel"]   = _settings.fontSizeLevel;
    doc["lineSpacingLevel"] = _settings.lineSpacingLevel;
    doc["fontFamily"]      = _settings.fontFamily;
    doc["sleepTimeoutMin"] = _settings.sleepTimeoutMin;
    doc["refreshEveryPages"] = _settings.refreshEveryPages;
    doc["wifiSSID"]        = _settings.wifiSSID;
    doc["wifiPass"]        = _settings.wifiPass;
    doc["showPageNumbers"] = _settings.showPageNumbers;
    doc["showBattery"]     = _settings.showBattery;
    doc["tapZoneLayout"]   = _settings.tapZoneLayout;
    doc["libraryViewMode"] = _settings.libraryViewMode;
    doc["librarySortOrder"] = _settings.librarySortOrder;
    doc["posterShowCovers"] = _settings.posterShowCovers;
    doc["frontlightEnabled"]    = _settings.frontlightEnabled;
    doc["frontlightBrightness"] = _settings.frontlightBrightness;
    doc["userButtonEnabled"]      = _settings.userButtonEnabled;
    doc["userButtonTapAction"]    = _settings.userButtonTapAction;
    doc["userButtonDoubleAction"] = _settings.userButtonDoubleAction;
    doc["userButtonLongAction"]   = _settings.userButtonLongAction;
    doc["bootButtonEnabled"]      = _settings.bootButtonEnabled;
    doc["bootButtonTapAction"]    = _settings.bootButtonTapAction;
    doc["bootButtonDoubleAction"] = _settings.bootButtonDoubleAction;
    doc["bootButtonLongAction"]   = _settings.bootButtonLongAction;

    // WP-3: KoSync fields. Normalize server scheme on save too so the
    // value persisted on SD matches what a hand-edited file would need
    // for the byte-compare check. User/key are re-validated; malformed
    // values are silently dropped to empty (callers in WP-5/WP-6
    // pre-validate, but defence in depth keeps malformed data out of
    // the persisted JSON).
    _settings.kosyncServer = normalize_https_scheme(_settings.kosyncServer);
    doc["kosyncServer"]     = _settings.kosyncServer;

    String saveUser = _settings.kosyncUser;
    if (saveUser.length() > 0 && !is_valid_kosync_user(saveUser)) {
        Serial.printf("[settings] kosyncUser invalid on save → persisting empty\n");
        saveUser = "";
        _settings.kosyncUser = "";
    }
    doc["kosyncUser"] = saveUser;

    String saveKey = _settings.kosyncKey;
    if (saveKey.length() > 0 && !is_valid_kosync_key(saveKey)) {
        Serial.printf("[settings] kosyncKey invalid on save → persisting empty\n");
        saveKey = "";
        _settings.kosyncKey = "";
    }
    doc["kosyncKey"] = saveKey;

    if (_settings.kosyncDeviceName.length() == 0) {
        _settings.kosyncDeviceName = compute_default_device_name();
    } else if (_settings.kosyncDeviceName.length() > 32) {
        _settings.kosyncDeviceName = _settings.kosyncDeviceName.substring(0, 32);
    }
    doc["kosyncDeviceName"] = _settings.kosyncDeviceName;

    if (doc.overflowed()) {
        // StaticJsonDocument silently truncates on overflow — refuse the
        // save rather than committing partial state to SD.
        Serial.println("Settings: JSON overflow — refusing save (bump pool size)");
        return false;
    }
    String json;
    serializeJson(doc, json);

    // Write to both stores so a firmware re-flash that leaves SD intact
    // (or, conversely, a missing SD on the next boot) still has a path
    // back to the user's settings.  Each store reports its own error;
    // we report overall success if either store accepted the write.
    bool sdOk  = storage_write_text_atomic(SETTINGS_PATH, SETTINGS_TMP, json);
    bool nvsOk = save_settings_text_to_nvs(json);

    // WP-3: mirror kosync fields to individual NVS keys alongside the
    // JSON blob. Best-effort: failure here doesn't fail the overall save
    // because the JSON mirror already carries the same data.
    {
        Preferences prefs;
        if (prefs.begin(NVS_NS, false)) {
            prefs.putString(NVS_KEY_KOSYNC_SRV, _settings.kosyncServer);
            prefs.putString(NVS_KEY_KOSYNC_USR, _settings.kosyncUser);
            prefs.putString(NVS_KEY_KOSYNC_KEY, _settings.kosyncKey);
            prefs.putString(NVS_KEY_KOSYNC_DEV, _settings.kosyncDeviceName);
            prefs.end();
        }
    }

    if (sdOk && nvsOk) {
        Serial.println("Settings: saved (SD+NVS)");
    } else if (sdOk) {
        Serial.println("Settings: saved (SD only — NVS write failed)");
    } else if (nvsOk) {
        Serial.println("Settings: saved (NVS only — SD write failed)");
    } else {
        Serial.println("Settings: save FAILED on both SD and NVS");
    }

    // WP-3: a successful save with values that passed the in-save validation
    // means the user has re-entered clean credentials — drop the runtime
    // invalid flag. Note: runtime-only; never serialized.
    if (sdOk || nvsOk) {
        _settings.kosyncCredentialsInvalid = false;
    }

    return sdOk || nvsOk;
}

Settings& settings_get() {
    return _settings;
}

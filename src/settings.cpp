#include "settings.h"
#include "config.h"
#include "storage_utils.h"
#include "button_action.h"
#include <SD.h>
#include <ArduinoJson.h>
#include <Preferences.h>

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

// Bump on any breaking schema change (renamed field, removed default,
// changed semantic of an existing field). Read by settings_init() so a
// future load can branch on it.
//   v3: replaced bool serifFont with uint8_t fontFamily (0=Sans/Lexend,
//       1=Serif/Literata, 2=Slab/Bitter).  Old bool migrates 1:1
//       (true → Serif).
static const int SETTINGS_SCHEMA_VERSION = 3;

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

// Parse a JSON blob and apply it to _settings.  Returns true on success.
// On schema-too-new or parse-error returns false and leaves _settings as
// the caller arranged (typically the defaults from settings_set_default()).
static bool apply_settings_json(const String& json, const char* sourceLabel) {
    StaticJsonDocument<1536> doc;
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

    // Both sources unusable.  Defaults already applied; persist them so
    // the next boot has *something* to load instead of running this same
    // fallback chain again.
    Serial.println("Settings: no usable source, writing defaults");
    if (!settings_save()) Serial.println("Settings: initial save failed");
}

bool settings_save() {
    // Pool sized for 26 fields + their string values; bump if you add more
    // keys (overflowed() check below catches the silent-truncate case).
    StaticJsonDocument<1536> doc;
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

    if (sdOk && nvsOk) {
        Serial.println("Settings: saved (SD+NVS)");
    } else if (sdOk) {
        Serial.println("Settings: saved (SD only — NVS write failed)");
    } else if (nvsOk) {
        Serial.println("Settings: saved (NVS only — SD write failed)");
    } else {
        Serial.println("Settings: save FAILED on both SD and NVS");
    }
    return sdOk || nvsOk;
}

Settings& settings_get() {
    return _settings;
}

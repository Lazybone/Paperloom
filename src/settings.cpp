#include "settings.h"
#include "config.h"
#include "storage_utils.h"
#include "button_action.h"
#include <SD.h>
#include <ArduinoJson.h>

static Settings _settings;
static const char* SETTINGS_PATH = "/books/.settings.json";
static const char* SETTINGS_TMP  = "/books/.settings.tmp";

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
}

void settings_init() {
    settings_set_default();

    File f = SD.open(SETTINGS_PATH, FILE_READ);
    if (!f) {
        Serial.println("Settings: no file found, using defaults");
        if (!settings_save()) Serial.println("Settings: initial save failed");
        return;
    }

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("Settings: parse error (%s), using defaults\n", err.c_str());
        if (!settings_save()) Serial.println("Settings: initial save failed");
        return;
    }

    // Read schemaVersion early so future migrations can branch on it.
    // If the file is from a NEWER firmware than we know about, the new
    // fields would silently fall back to defaults (data loss on rollback).
    // Refuse to load and re-write defaults so the user notices.
    int loadedSchema = doc["schemaVersion"] | 1;
    if (loadedSchema > SETTINGS_SCHEMA_VERSION) {
        Serial.printf("Settings: schema %d > current %d — saving defaults to avoid corruption\n",
                      loadedSchema, SETTINGS_SCHEMA_VERSION);
        if (!settings_save()) Serial.println("Settings: defaults save failed");
        return;
    }
    if (loadedSchema != SETTINGS_SCHEMA_VERSION) {
        // Older firmware wrote schema < SETTINGS_SCHEMA_VERSION; the
        // per-field migration paths below ("else if doc.containsKey"
        // fallbacks) bring the in-RAM Settings up to current.  The next
        // settings_save() will rewrite the file at the new version.
        Serial.printf("Settings: schema=%d → %d, applying field-level migrations\n",
                      loadedSchema, SETTINGS_SCHEMA_VERSION);
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

    // Don't log the SSID — even though it's not a credential, it identifies
    // the user's home network on the serial port. Length-only is enough to
    // confirm the load worked.
    Serial.printf("Settings: loaded (fontLevel=%d, family=%u, sleep=%dmin, refresh=%d, wifi_len=%u)\n",
                  _settings.fontSizeLevel, (unsigned)_settings.fontFamily,
                  _settings.sleepTimeoutMin,
                  _settings.refreshEveryPages,
                  (unsigned)_settings.wifiSSID.length());
}

bool settings_save() {
    // Pool sized for 22 fields + their string values; bump if you add more
    // keys (overflowed() check below catches the silent-truncate case).
    StaticJsonDocument<1280> doc;
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

    if (doc.overflowed()) {
        // StaticJsonDocument silently truncates on overflow — refuse the
        // save rather than committing partial state to SD.
        Serial.println("Settings: JSON overflow — refusing save (bump pool size)");
        return false;
    }
    String json;
    serializeJson(doc, json);
    if (storage_write_text_atomic(SETTINGS_PATH, SETTINGS_TMP, json)) {
        Serial.println("Settings: saved atomically");
        return true;
    }
    Serial.println("Settings: atomic save failed");
    return false;
}

Settings& settings_get() {
    return _settings;
}

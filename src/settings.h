#pragma once

#include <Arduino.h>

struct Settings {
    // Display
    int   fontSize;         // DEPRECATED — mapped to fontSizeLevel on load
    int   fontSizeLevel;    // 0=XS, 1=S, 2=M(default), 3=ML, 4=L
    uint8_t lineSpacingLevel; // 0=Compact, 1=Normal, 2=Relaxed(default), 3=Spacious, 4=Extra
    // Reader font family. Values: 0=Sans(Lexend Deca), 1=Serif(Literata),
    // 2=Slab(Bitter). Replaced the pre-v3 bool serifFont; see
    // settings.cpp for the on-disk migration path.
    uint8_t fontFamily;
    uint16_t sleepTimeoutMin;  // minutes before deep sleep (default: 5; uint16 prevents negative-wrap)
    int   refreshEveryPages; // stronger cleanup cadence during reading

    // WiFi
    String wifiSSID;
    String wifiPass;

    // Reading
    bool   showPageNumbers;  // footer page numbers (default: true)
    bool   showBattery;      // battery in header (default: true)
    int    tapZoneLayout;    // 0=left/center/right (default), 1=top/mid/bottom
    int    libraryViewMode;  // 0=list, 1=poster
    uint8_t librarySortOrder; // 0=title, 1=author, 2=recent, 3=size
    bool   posterShowCovers; // experimental: render EPUB cover art in poster view

    // Frontlight (T5S3 PRO only — ignored on other targets)
    bool    frontlightEnabled;     // master on/off
    uint8_t frontlightBrightness;  // 0..100 percent

    // User button (PCA9535 IO12 on T5S3 PRO). Action per gesture; values from
    // ButtonAction enum in button_action.h. Stored as uint8_t for simple JSON.
    bool    userButtonEnabled;         // master toggle — polling is off unless this is true
    uint8_t userButtonTapAction;       // single short press
    uint8_t userButtonDoubleAction;    // two short presses within window
    uint8_t userButtonLongAction;      // press-and-hold

    // Boot button (GPIO 0). Same configurable gesture model as the user
    // button. Long-press always falls back to BTN_ACTION_SLEEP when
    // bootButtonEnabled=false so the device can always be powered down.
    // ext1 wake-source on GPIO 0 is unconditional, independent of this flag.
    bool    bootButtonEnabled;
    uint8_t bootButtonTapAction;
    uint8_t bootButtonDoubleAction;
    uint8_t bootButtonLongAction;

    // KoSync (WP-3) — progress-sync server credentials. The plaintext
    // password NEVER reaches this layer; callers in WP-5 / WP-6 hash it
    // (MD5 hex, 32 lowercase chars) before invoking settings_save().
    String   kosyncServer;             // https://-only; default "https://kosync.eu" when never set
    String   kosyncUser;               // [A-Za-z0-9_.-]{1,32}; empty if unconfigured
    String   kosyncKey;                // MD5 hex of password (32 lowercase hex chars); empty if unconfigured
    String   kosyncDeviceName;         // ≤32 chars; defaults to "paperloom-<last4-MAC>" on first load
    bool     kosyncCredentialsInvalid; // Runtime-only flag (NOT persisted): set on load when stored values fail validation
};

void settings_init();           // Load from SD or create defaults
bool settings_save();           // Write to SD as JSON; false on IO failure
Settings& settings_get();       // Reference to live settings
void settings_set_default();    // Reset to factory defaults

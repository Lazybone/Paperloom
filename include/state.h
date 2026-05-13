#pragma once

// App state machine
enum AppState {
    STATE_BOOT,
    STATE_LIBRARY,
    STATE_READER,
    STATE_WIFI,
    STATE_MENU,         // redesigned reader overlay menu
    STATE_GOTO,         // go to approximate page or percentage
    STATE_TOC,          // table of contents
    STATE_BOOKMARKS,    // bookmark list
    STATE_SETTINGS,     // settings page
    STATE_OTA_CHECK,    // OTA update check / download
    STATE_WIFI_SETUP,   // WiFi network scan + selection
    STATE_WIFI_KEYBOARD, // On-screen keyboard for SSID/password entry
    STATE_KOSYNC_SETUP,        // on-device kosync credential entry
    STATE_SYNC_CONFLICT,       // local-vs-remote conflict resolution dialog (WP-9)
    STATE_KOSYNC_PIN_PROMPT    // PIN displayed on e-paper during web-UI credential write (WP-6c)
};

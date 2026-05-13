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
    STATE_KOSYNC_PIN_PROMPT,   // PIN displayed on e-paper during web-UI credential write (WP-6c)
    // ─── TRIGGER values (NOT real UI states) ───────────────────────────
    // STATE_SLEEP_REQUEST is a transient sentinel returned from
    // ui_reader_menu_touch when the user taps the "Sleep" entry. It is
    // intercepted by handleMenuTouch in main.cpp, which calls
    // enterDeepSleep(true) directly. The global `appState` variable must
    // NEVER be assigned this value — otherwise the sleepState persistence
    // path in enterDeepSleep would store it in Preferences, causing a
    // bogus resume target on the next wake.
    //
    // Append-only: do NOT reorder existing values. The integer encoding
    // is persisted in Preferences ("sleepState") and read back across
    // reboots; reordering would silently remap saved resume states.
    STATE_SLEEP_REQUEST
};

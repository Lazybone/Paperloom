#include "button_action.h"

#include "kosync_sync.h"
#include "ui/ui_toast.h"
#include "ui/ui_reader_sync_conflict.h"
#include "settings.h"
#include "../include/state.h"

const char* button_action_name(uint8_t action) {
    switch (action) {
        case BTN_ACTION_NONE:             return "None";
        case BTN_ACTION_BACKLIGHT_TOGGLE: return "Light toggle";
        case BTN_ACTION_LIBRARY:          return "Library";
        case BTN_ACTION_SLEEP:            return "Sleep";
        case BTN_ACTION_NEXT_PAGE:        return "Next page";
        case BTN_ACTION_PREV_PAGE:        return "Prev page";
        case BTN_ACTION_MENU:             return "Menu";
        case BTN_ACTION_KOSYNC_SYNC:      return "Sync Fortschritt";
        default:                          return "?";
    }
}

// button_action_execute is implemented in main.cpp so it can dispatch into
// the file-local static functions (page nav, sleep, state changes).
//
// WP-8: button_action_kosync_sync() is a public helper that mirrors the
// reader-menu "Sync Fortschritt" flow. It is implemented here (rather than
// inline in main.cpp's dispatcher) so the coordinator + UI dependencies stay
// out of main.cpp and so it can be unit-tested in isolation. The dispatcher
// in main.cpp must add:
//   case BTN_ACTION_KOSYNC_SYNC: button_action_kosync_sync(); break;
extern void setAppState(AppState state);
extern AppState getAppState();
extern void setNeedsRedraw(bool);

void button_action_kosync_sync() {
    // Mirror the reader-menu Sync flow from WP-7.
    if (!kosync_is_coordinator_initialized()) {
        ui_toast_show("Sync error", 2000, true);
        return;
    }
    // Guard: only fire from reader / reader-menu context. Ignore button
    // while in modal states (library, conflict dialog, settings, etc.) so
    // a stray press cannot trigger sync from the wrong screen.
    AppState cur = getAppState();
    if (cur != STATE_READER && cur != STATE_MENU) {
        return;
    }
    // Settings precheck (mirror reader-menu)
    const Settings& s = settings_get();
    if (s.kosyncServer.length() == 0 || s.kosyncUser.length() == 0 ||
        s.kosyncKey.length() != 32 || s.kosyncCredentialsInvalid) {
        ui_toast_show("KoSync nicht konfiguriert", 2500, true);
        return;
    }
    // Note: book-open check is performed inside coordinator.beginSync() via
    // reader (it calls reader.getDocumentHash() which returns "" when no
    // book is open). No additional guard needed here.
    String toast;
    if (!kosync_get_coordinator().beginSync(toast)) {
        ui_toast_show(toast, 2500, true);
        return;
    }
    setAppState(STATE_SYNC_PROGRESS);
    setNeedsRedraw(true);
}

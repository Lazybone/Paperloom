#pragma once

#include "state.h"

// On-device KoSync credential setup screen (WP-5).
//
// Three fields are entered via an inline on-screen keyboard:
//   1) Server URL (https://-only)
//   2) Username   ([A-Za-z0-9_.-]{1,32})
//   3) Password   (any chars, length 1..128)
//
// Plaintext password lives only in this module's memory. On [Speichern] it
// is MD5-hashed (32-char lowercase hex), the plaintext buffer is zeroed,
// and only the hash is persisted via settings_save().
//
// main.cpp dispatch:
//   - on entry to STATE_KOSYNC_SETUP, call ui_kosync_setup_enter().
//   - draw  : ui_kosync_setup_draw()
//   - touch : ui_kosync_setup_touch(x, y) — returns the next AppState
//             (STATE_KOSYNC_SETUP while still editing,
//              STATE_READER on save/cancel).

void ui_kosync_setup_enter();
void ui_kosync_setup_draw();
AppState ui_kosync_setup_touch(int x, int y);

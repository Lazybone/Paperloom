// ui_kosync_pin_prompt.h — Out-of-band PIN display for credential writes (WP-6c).
//
// Renders the 6-digit PIN issued by kosync_pin_state when a web-UI client
// posts to /api/kosync-settings or /api/kosync-register without (or with
// an invalid) `pin` field. The PIN is displayed on the e-paper as an
// out-of-band confirmation channel; the user reads it from the device and
// types it into the browser.
//
// State flow:
//   * apply_pin_gate() in kosync_http_handlers calls kosync_pin_generate(),
//     which sets g_kosyncShowPinRequested = true.
//   * main.cpp's draw dispatcher consumes the flag with exchange(false)
//     and transitions AppState → STATE_KOSYNC_PIN_PROMPT.
//   * ui_kosync_pin_prompt_tick() returns STATE_READER once the PIN is
//     consumed (Ok validation) or expires.
//   * The cancel button explicitly resets the PIN state and returns to
//     the reader.
#pragma once

#include "../../include/state.h"

// Called by main.cpp draw dispatcher when AppState == STATE_KOSYNC_PIN_PROMPT.
void ui_kosync_pin_prompt_draw();

// Called by main.cpp touch dispatcher. Returns the new AppState — only the
// [Abbrechen] tap zone is interactive; everything else keeps the prompt up
// until the PIN is consumed by an HTTP request or expires.
AppState ui_kosync_pin_prompt_touch(int x, int y);

// Called by main.cpp at the top of each loop iteration while in this state.
// Auto-exits to STATE_READER when the PIN has been consumed or expired
// (shows an "expired" toast in the latter case). Returns the new AppState.
AppState ui_kosync_pin_prompt_tick();

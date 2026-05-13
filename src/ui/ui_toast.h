#pragma once

#include <Arduino.h>

// ─── Toast / status banner primitive ─────────────────────────────────
//
// A minimal bottom-of-screen banner used to surface short status messages
// (sync results, settings actions, etc.) without blocking the main UI.
//
// Lifecycle:
//   * ui_toast_show(...)      — schedule a banner, draw it immediately as
//                               a partial-update overlay (latest wins).
//   * ui_toast_tick()         — call from the top of the main loop on
//                               every iteration. When the toast has
//                               expired, marks it inactive and requests
//                               a redraw of the underlying screen state.
//   * ui_toast_is_active()    — true while the banner is on screen;
//                               useful for input dispatchers that want
//                               to treat the next touch as a dismiss.
//
// The toast renders generically — it has no knowledge of any specific
// feature (sync, settings, OTA, …). Any subsystem may call it.

void ui_toast_show(const String& msg, uint32_t durationMs = 2000, bool isError = false);
void ui_toast_tick();
bool ui_toast_is_active();

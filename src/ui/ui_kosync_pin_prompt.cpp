// ui_kosync_pin_prompt.cpp — PIN display screen for web-UI credential writes (WP-6c).
//
// Layout (portrait 540 x 960):
//   ┌─────────────────────────────────────────────┐
//   │  PIN-Bestätigung                  [batt]    │  header
//   ├─────────────────────────────────────────────┤
//   │                                             │
//   │     Bitte den folgenden PIN im Browser      │
//   │              eingeben:                      │
//   │                                             │
//   │                                             │
//   │              1 2 3 4 5 6                    │  large numeric
//   │                                             │
//   │                                             │
//   │           Gültig noch 42 Sekunden           │  countdown
//   │                                             │
//   ├─────────────────────────────────────────────┤
//   │                [Abbrechen]                  │  footer
//   └─────────────────────────────────────────────┘
//
// Privacy & security: the PIN is intentionally large and on the e-paper.
// It is NEVER logged. Serial.printf in this file is restricted to
// non-content state markers.

#include "ui_kosync_pin_prompt.h"

#include "ui_common.h"
#include "ui_toast.h"
#include "config.h"
#include "../display.h"
#include "../kosync_pin_state.h"

#include <Arduino.h>
#include <stdio.h>

namespace {

// ─── Layout constants (portrait 540 x 960) ─────────────────────────
constexpr int W = PORTRAIT_W;
constexpr int H = PORTRAIT_H;

constexpr int BODY_TOP   = HEADER_HEIGHT + MARGIN_Y;
constexpr int FOOTER_TOP = H - FOOTER_HEIGHT;

// Footer is a single full-width [Abbrechen] button (mirrors the cancel
// slot of ui_reader_sync_conflict).
constexpr int BTN_CANCEL_X = 0;
constexpr int BTN_CANCEL_W = W;

// Vertical anchor for the PIN itself: centred in the body region.
constexpr int PIN_CENTER_Y = (BODY_TOP + FOOTER_TOP) / 2;

// ─── Module state ──────────────────────────────────────────────────
//
// We track whether a redraw is needed for the per-second countdown so
// the e-paper isn't flashed every loop iteration. Last drawn second is
// stored so tick() can decide.
int8_t   g_lastShownSecond  = -1;
bool     g_wasActiveLastTick = false;  // edge-detect expiry → toast

// ─── Helpers ───────────────────────────────────────────────────────

// Format a six-digit PIN with a space between groups of three for
// readability: "123 456". The PIN value is already 0..999999.
void format_pin(uint32_t pin, char* out, size_t outLen) {
    if (outLen == 0) return;
    snprintf(out, outLen, "%03u %03u", pin / 1000u, pin % 1000u);
}

void draw_centered(int y, const char* text, uint8_t fg) {
    const int tw = display_text_width(text);
    display_draw_text((W - tw) / 2, y, text, fg);
}

void draw_footer_button(const char* label) {
    display_draw_filled_rect(BTN_CANCEL_X, FOOTER_TOP, BTN_CANCEL_W,
                             FOOTER_HEIGHT, 2);
    const int tw = display_text_width(label);
    display_draw_text((W - tw) / 2,
                      FOOTER_TOP + FOOTER_HEIGHT - 14,
                      label, 15);
}

}  // namespace

// ─── Public API ────────────────────────────────────────────────────

void ui_kosync_pin_prompt_draw() {
    display_fill_screen(15);
    display_set_font_size(2);
    drawHeader("PIN-Bestätigung");

    // Decide which body variant to render: locked / active-PIN / expired.
    uint32_t lockoutRemMs = 0;
    const bool locked = kosync_pin_is_locked_out(&lockoutRemMs);
    const bool active = !locked && kosync_pin_is_active();

    if (locked) {
        // Lockout state: show a clear message + remaining minutes, no PIN.
        display_set_font_size(2);
        draw_centered(BODY_TOP + 60, "Zu viele Fehlversuche", 0);

        char buf[64];
        const uint32_t mins = (lockoutRemMs + 59999u) / 60000u;
        snprintf(buf, sizeof(buf), "Bitte %u Min. warten",
                 static_cast<unsigned>(mins));
        draw_centered(PIN_CENTER_Y, buf, 0);

        // Small footnote so the user knows what to do after the wait.
        display_set_font_size(1);
        draw_centered(PIN_CENTER_Y + 50,
                      "Anschluss erneut im Browser starten.", 6);
    } else if (active) {
        // Hero PIN line — largest available font, centred.
        display_set_font_size(1);
        draw_centered(BODY_TOP + 30,
                      "Bitte den folgenden PIN im Browser eingeben:", 0);

        char pinBuf[16];
        format_pin(kosync_pin_current_value(), pinBuf, sizeof(pinBuf));
        display_set_font_size(4);
        const int pinW = display_text_width(pinBuf);
        display_draw_text((W - pinW) / 2, PIN_CENTER_Y, pinBuf, 0);

        // Countdown line.
        display_set_font_size(1);
        const uint32_t remMs = kosync_pin_remaining_ms();
        const uint32_t remSec = (remMs + 999u) / 1000u;
        char tBuf[48];
        snprintf(tBuf, sizeof(tBuf), "Gültig noch %u Sekunden",
                 static_cast<unsigned>(remSec));
        draw_centered(PIN_CENTER_Y + 80, tBuf, 6);
        g_lastShownSecond = static_cast<int8_t>(remSec % 60);
    } else {
        // Neither locked nor active → PIN expired or consumed. The tick
        // handler will route us out of this state on the next iteration;
        // render a transient message so we don't blank between frames.
        display_set_font_size(2);
        draw_centered(PIN_CENTER_Y, "PIN abgelaufen", 0);
    }

    draw_footer_button("Abbrechen");

    display_begin_frame();
    display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
    display_flush();

    g_wasActiveLastTick = active;
}

AppState ui_kosync_pin_prompt_touch(int x, int y) {
    (void)x;
    // Only the bottom button bar reacts. Taps elsewhere keep the prompt up.
    if (y < FOOTER_TOP) {
        return STATE_KOSYNC_PIN_PROMPT;
    }

    // [Abbrechen] — reset the PIN gate and drop back to the reader.
    Serial.println("[kosync-pin] user cancelled prompt");
    kosync_pin_reset_state();
    g_lastShownSecond  = -1;
    g_wasActiveLastTick = false;
    return STATE_READER;
}

AppState ui_kosync_pin_prompt_tick() {
    // Lockout: keep the user on the screen so they see the wait time.
    uint32_t lockoutRemMs = 0;
    if (kosync_pin_is_locked_out(&lockoutRemMs)) {
        return STATE_KOSYNC_PIN_PROMPT;
    }

    // PIN was active a moment ago and is no longer → either consumed or
    // expired. Distinguish by remaining time: if remaining_ms() is 0
    // AND there's no value, it expired without consumption. The HTTP
    // handler clears the value on Ok and on Expired alike, so we cannot
    // distinguish perfectly — but if the user is still here when the
    // PIN goes inactive, "expired" is the user-visible interpretation.
    if (!kosync_pin_is_active()) {
        if (g_wasActiveLastTick) {
            // Edge: active → inactive while the user was looking. Toast
            // them with the most likely reason.
            ui_toast_show("PIN abgelaufen", 2000, false);
        }
        g_lastShownSecond  = -1;
        g_wasActiveLastTick = false;
        return STATE_READER;
    }

    // Active: redraw at most once per second to update the countdown.
    const uint32_t remMs = kosync_pin_remaining_ms();
    const int8_t   sec   = static_cast<int8_t>(((remMs + 999u) / 1000u) % 60u);
    if (sec != g_lastShownSecond) {
        ui_kosync_pin_prompt_draw();
    }
    g_wasActiveLastTick = true;
    return STATE_KOSYNC_PIN_PROMPT;
}

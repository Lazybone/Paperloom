#include "ui_toast.h"
#include "../display.h"
#include "config.h"

// Forward-decl of the main-loop redraw signal. main.cpp owns the global
// `needsRedraw` flag; ui code requests an underlying redraw via this
// setter (same pattern used in src/ui/ui_reader.cpp).
extern void setNeedsRedraw(bool val);

namespace {

// ─── Layout constants ────────────────────────────────────────────────
// UI font advance_y (medium reader font), matching the FONT_H used in
// main.cpp and the "Continue Reading" banner in ui_library.cpp.
constexpr int kFontH      = 50;
constexpr int kBannerH    = kFontH + 16;   // ~66 px — matches drawHeader band
constexpr int kMarginX    = MARGIN_X;

// Grey-4 colour codes used by the epdiy display layer:
//   0  = black
//   15 = white
// Success banner: white fill, black text + thin top rule (matches the
// "Continue Reading" banner pattern in ui_library.cpp:266).
// Error banner: black fill, white text (inverted) so it stays
// distinguishable from a normal info banner under partial waveforms.
constexpr uint8_t kFillSuccess   = 15;
constexpr uint8_t kTextSuccess   = 0;
constexpr uint8_t kRuleSuccess   = 10;

constexpr uint8_t kFillError     = 0;
constexpr uint8_t kTextError     = 15;
constexpr uint8_t kRuleError     = 5;

// Cap displayed message at this length; longer strings are truncated
// with an ellipsis. 64 keeps the banner readable at medium font on a
// 540-px-wide panel without expensive per-char width measurement.
constexpr size_t kMaxMsgChars = 64;

// ─── Toast state ─────────────────────────────────────────────────────
// Single-threaded main-loop invariant: Paperloom runs one cooperative
// loop, so no atomics or locks are needed around this flag.
struct ToastState {
    String   msg;
    uint32_t startedAtMs = 0;
    uint32_t durationMs  = 0;
    bool     isError     = false;
};

ToastState g_toast;
bool       g_active = false;

// Truncate to kMaxMsgChars with an ellipsis suffix. Operates on a copy
// so the original caller String is left untouched.
String truncate_for_banner(const String& in) {
    if (in.length() <= kMaxMsgChars) {
        return in;
    }
    // Reserve 3 chars for the ellipsis.
    return in.substring(0, kMaxMsgChars - 3) + "...";
}

// Render the banner immediately using the partial-update overlay path.
// Does NOT begin/flush the underlying screen — the toast is purely an
// overlay layer on top of whatever was already drawn.
void draw_banner() {
    const int W = display_width();
    const int H = display_height();
    const int y = H - kBannerH;

    const uint8_t fill = g_toast.isError ? kFillError   : kFillSuccess;
    const uint8_t text = g_toast.isError ? kTextError   : kTextSuccess;
    const uint8_t rule = g_toast.isError ? kRuleError   : kRuleSuccess;

    // Chrome text always uses the UI font (Inter) at medium size,
    // matching drawHeader / the OTA "Waking..." banner in main.cpp.
    display_set_font_size(2);

    display_draw_filled_rect(0, y, W, kBannerH, fill);
    display_draw_hline(0, y, W, rule);

    const String shown = truncate_for_banner(g_toast.msg);
    const int textW = display_text_width(shown.c_str());
    int textX = (W - textW) / 2;
    if (textX < kMarginX) {
        textX = kMarginX;  // never overflow the left margin
    }
    // Baseline placement mirrors the "Continue Reading" banner:
    // bannerY = y + bannerH/2 + 2 keeps the medium-size glyphs visually
    // centred in the band.
    const int textY = y + kBannerH / 2 + 2;
    display_draw_text(textX, textY, shown.c_str(), text);

    // Partial-update overlay flush: one waveform update on the banner
    // strip only — leaves the rest of the screen latched.
    display_set_overlay_rect(0, y, W, kBannerH);
    display_begin_frame();
    display_mark_dirty(Zone::Overlay, ChangeKind::StructuralRedraw);
    display_flush();
}

}  // namespace

// ─── Public API ──────────────────────────────────────────────────────

void ui_toast_show(const String& msg, uint32_t durationMs, bool isError) {
    // Latest-wins: overwrite any in-flight toast. Clamp duration to a
    // sane minimum so a 0-ms call still flashes long enough to be seen
    // (and so the tick-based expiry doesn't fire on the same loop pass).
    if (durationMs < 250) {
        durationMs = 250;
    }

    g_toast.msg          = msg;
    g_toast.startedAtMs  = millis();
    g_toast.durationMs   = durationMs;
    g_toast.isError      = isError;
    g_active             = true;

    draw_banner();
}

void ui_toast_tick() {
    if (!g_active) {
        return;
    }

    // Wrap-safe timeout check: compute the delta as a signed int32_t so
    // millis() rollover (every ~49.7 days) doesn't immediately expire
    // an in-flight toast. The cast forces a modular subtraction.
    const uint32_t now   = millis();
    const int32_t  delta = static_cast<int32_t>(now - g_toast.startedAtMs);
    if (delta >= 0 && static_cast<uint32_t>(delta) >= g_toast.durationMs) {
        g_active = false;
        // Ask the main loop to redraw the underlying screen so the
        // banner strip is repainted with the real UI behind it.
        setNeedsRedraw(true);
    }
}

bool ui_toast_is_active() {
    return g_active;
}

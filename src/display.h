#pragma once

#include <Arduino.h>
#include "config.h"  // FONT_FAMILY_COUNT, FONT_SIZE_LEVEL_COUNT

void display_init();
void display_clear();
void display_fill_screen(uint8_t gray4);
void display_draw_text(int x, int y, const char* text, uint8_t fg_color = 0);
void display_draw_pixel(int x, int y, uint8_t gray4);
void display_draw_filled_rect(int x, int y, int w, int h, uint8_t gray4);
void display_draw_hline(int x, int y, int w, uint8_t gray4);
void display_draw_vline(int x, int y, int h, uint8_t gray4);
void display_draw_rect(int x, int y, int w, int h, uint8_t gray4);
// ─── Legacy display_update* shims ─────────────────────────────────────
// These now route through the intent-based partial-update API below.
// Each is [[deprecated]] so any remaining call site shows a build
// warning. Migrate to display_begin_frame() + display_mark_dirty(Zone,
// ChangeKind) + display_flush() with an explicit zone+intent.
//
// NOTE on _medium specifically: pre-refactor it was a 2-cycle partial.
// The shim now escalates to WakeFull (6-cycle GC16 full clear) — a
// 3× perf regression. Migrate _medium call sites first.
//
// display_update_sleep() is NOT deprecated — it is a special terminal
// path (sleep image latch) that intentionally bypasses the intent API.
[[deprecated("Routes through WakeFull (6-cycle GC16). Use display_mark_dirty(Zone::FullScreen, ChangeKind::WakeFull) + display_flush() with an explicit zone+intent.")]]
void display_update();               // full refresh (heavy clear + draw, ~3s, 6 cycles)
void display_update_sleep();         // full refresh for sleep image; preserves panel hold state
[[deprecated("Silently maps to WakeFull (6-cycle GC16 full clear) — 3x perf regression vs the pre-refactor 2-cycle partial. Use display_mark_dirty + display_flush with the correct ChangeKind for the surface (TextReflow / StructuralRedraw).")]]
void display_update_medium();        // medium refresh for chapter jumps (~1s, 2 cycles)
[[deprecated("Maps to GL16 full-screen partial. Use display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw) + display_flush() directly.")]]
void display_update_fast();          // lighter full-screen refresh for page turns (1 cycle)
[[deprecated("Use display_mark_dirty(Zone::ReaderBody, ChangeKind::TextReflow [or StructuralRedraw]) + display_flush() directly.")]]
void display_update_reader_body(int x, int y, int w, int h, bool strongCleanup = false);
[[deprecated("Use display_mark_dirty on the actual dirty zones + display_flush() with explicit ChangeKind.")]]
void display_update_partial();        // partial update (no clear, no flash)
[[deprecated("Use display_mark_dirty + display_flush directly with an explicit ChangeKind instead of a fullRefresh boolean.")]]
void display_update_mode(bool fullRefresh);  // select mode
int  display_text_width(const char* text);
int  display_font_height();
int  display_font_ascender();
int  display_width();
int  display_height();

// =====================================================================
// Partial-Update API (intent-based)
// =====================================================================
//
// Lifecycle per frame:
//   display_begin_frame();
//   ... draw into the portrait framebuffer ...
//   display_mark_dirty(Zone::ReaderBody, ChangeKind::TextReflow);
//   ... optional: display_mark_dirty(...) for additional zones ...
//   display_flush();   // atomic: one poweron, one update per dirty zone
//
// Convention: UI callers describe INTENT (ChangeKind) — the display layer
// chooses the matching epdiy waveform mode and anti-ghost cadence. UI code
// must not poke MODE_GC16/MODE_GL16 directly.
//
// `needsRedraw` flag in main.cpp is orthogonal: it signals that a draw
// pass is required. Which zones become dirty is decided by the specific
// UI draw function via display_mark_dirty().
//
// The counter `framesSinceFullRefresh` (tracked internally) is updated
// exclusively inside display_flush(). When it reaches the
// REFRESH_INTERVAL_READER threshold from include/config.h, the next flush
// is auto-escalated to GC16 full refresh and the counter resets.
//
// Legacy display_update*() entry points above are now thin wrappers that
// internally call this API, so existing callers keep working until they
// migrate to explicit zone+intent in later work packages.
// =====================================================================
//
// ─── Intent-based partial-update API ──────────────────────────────────
// UI callers declare what kind of change happened; the display layer maps
// that intent to the right epdiy waveform mode and tracks anti-ghost
// cadence (forced GC16 every REFRESH_INTERVAL_READER frames).
enum class ChangeKind : uint8_t {
    GlyphTick,         // Battery icon redraw, clock tick    → DU4 (fast 4-grey)
    HighlightToggle,   // Button / row selection             → DU4
    TapPulse,          // Single-shot press feedback         → DU4 (same 4-grey, ~50–80 ms)
    TextReflow,        // Page-turn body, footer text        → GL16 (non-flashing)
    StructuralRedraw,  // Tab / screen change, overlay open  → GL16
    WakeFull,          // Wake from sleep — counter reset    → GC16 full + clear
    SleepImage,        // Sleep image — preserve hold state  → GC16 (special path)
    AntiGhost          // Forced periodic clean refresh      → GC16 full + clear
};

// Reader screen is split into three fixed zones; non-reader screens
// (Library, Settings, Wifi setup, …) typically use Zone::FullScreen.
// Zone::Overlay has a dynamic rect set via display_set_overlay_rect().
enum class Zone : uint8_t {
    ReaderHeader = 0,   // {0,   0, 540,  66}   — battery, title bar
    ReaderBody   = 1,   // {0,  82, 540, 828}   — text body
    ReaderFooter = 2,   // {0, 910, 540,  50}   — page nr, progress
    // Reserved for partial-overlay use cases (e.g. settings picker
    // dropdown). Currently unused — all in-app overlays render via
    // Zone::FullScreen. If you mark this zone dirty, call
    // display_set_overlay_rect() FIRST every frame the overlay is dirty
    // (the rect is reset to empty after each display_flush()).
    Overlay      = 3,   // dynamic, set by caller
    FullScreen   = 4,   // {0,   0, 540, 960}   — whole portrait surface
    _Count       = 5
};

// Begin a new frame: clears all per-zone dirty flags. Call this once per
// UI redraw, before any display_mark_dirty().
void display_begin_frame();

// Mark a zone as dirty with the given intent. Multiple marks on the same
// zone in one frame keep the strongest intent (WakeFull/AntiGhost win
// over StructuralRedraw, which wins over TextReflow, which wins over
// GlyphTick/HighlightToggle).
void display_mark_dirty(Zone z, ChangeKind k);

// Set the rect for Zone::Overlay before display_flush(). Coordinates are
// portrait-space (0..540, 0..960). The rect is reset to empty after each
// flush, so it must be set every frame the overlay is dirty.
void display_set_overlay_rect(int x, int y, int w, int h);

// Push all dirty zones to the panel atomically: one waveform update per
// dirty zone, batched between a single epd_be_poweron() / poweroff_all()
// pair. Anti-ghost auto-upgrade kicks in after REFRESH_INTERVAL_READER
// consecutive partial frames.
void display_flush();

// Force a full GC16 refresh of the entire screen and reset the partial
// counter. Use after sleep wake or when the panel is visibly degraded.
//
// WARNING: This helper is fire-and-forget — it calls display_begin_frame()
// internally, which clears every zone's dirty flag. Therefore it MUST NOT
// be called after the caller has already done its own
// display_begin_frame() + display_mark_dirty(...) sequence. In that case
// the zone marks would be silently discarded. Drive display_flush()
// directly instead and mark at least one zone with ChangeKind::WakeFull
// (or AntiGhost / SleepImage) — the full-clear path inside display_flush()
// will run and reset every per-zone anti-ghost counter to 0 just like this helper.
void display_force_full_refresh();

// Font family identifiers — keep in sync with settings.h's fontFamily and
// the static font lookup tables in display.cpp.  Stored as uint8_t in
// settings so the on-disk schema stays compact.  FONT_FAMILY_COUNT lives
// in include/config.h so non-display code (settings.cpp) can clamp it
// without pulling in this header.
enum FontFamily : uint8_t {
    FONT_FAMILY_SANS  = 0,  // Lexend Deca — research-backed sans
    FONT_FAMILY_SERIF = 1,  // Literata — Google's screen-optimized serif
    FONT_FAMILY_SLAB  = 2,  // Bitter — slab serif tuned for e-ink
    FONT_FAMILY_INTER = 3,  // Inter — also used for UI chrome; available
                            // here so readers can pick a single-font device.
    FONT_FAMILY_CHAREINK = 4,  // ChareInk7SP — SIL Charis derivative tuned
                               // for e-ink legibility (high contrast strokes).
};

// Display names for each family.  Defined in display.cpp; declared here so
// UI code can label dropdowns without duplicating the string table.
extern const char* const kFontFamilyNames[FONT_FAMILY_COUNT];

void display_set_font_size(int sizeLevel);             // 0-4: XS,S,M,ML,L (defaults to Sans family)
void display_set_font(int sizeLevel, uint8_t family);  // sizeLevel + FontFamily index
void display_power_off();             // ensure EPD power rail is off

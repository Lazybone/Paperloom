// ui_reader_sync_conflict.cpp — KoSync conflict-resolution dialog (WP-9).
//
// Renders a side-by-side comparison of local vs. remote reading progress
// when the sync coordinator returns SyncResult::hasConflict = true, and
// captures the user's decision (keep local / take remote / cancel).
//
// State flow:
//   main.cpp dispatcher calls ui_sync_conflict_set_data(result) before
//   transitioning to STATE_SYNC_CONFLICT. Subsequent draw + touch calls
//   read the cached SyncResult. On a terminal tap we route the decision
//   to KosyncSyncCoordinator::resolveConflict() / clearBusy(), show a
//   toast, and return STATE_READER.
//
// Privacy:
//   Progress contents (chapter / page / percentage) are PII-adjacent
//   reading-position data. We render them on-screen but must NOT log
//   them at INFO level. Serial.printf usage in this file is limited to
//   non-content control-flow markers.

#include "ui_reader_sync_conflict.h"

#include "ui_common.h"
#include "ui_toast.h"
#include "config.h"
#include "../display.h"
#include "../kosync_sync.h"

#include <Arduino.h>
#include <stdio.h>
#include <time.h>

// ─── Layout constants (portrait 540 x 960) ─────────────────────────
namespace {

constexpr int W = PORTRAIT_W;
constexpr int H = PORTRAIT_H;

// Body region below header, above button bar.
constexpr int BODY_TOP = HEADER_HEIGHT + MARGIN_Y;

// Bottom button bar (mirrors ui_reader_kosync_setup footer style).
constexpr int FOOTER_TOP = H - FOOTER_HEIGHT;

// Three-button split across the full panel width:
//   [Lokal behalten] [Remote übernehmen] [Abbrechen]
// The cancel button gets the smallest slice (~22%), the two actions split
// the remaining width roughly evenly.
constexpr int BTN_CANCEL_W = 120;
constexpr int BTN_LOCAL_W  = (W - BTN_CANCEL_W) / 2;
constexpr int BTN_REMOTE_W = W - BTN_CANCEL_W - BTN_LOCAL_W;
static_assert(BTN_LOCAL_W + BTN_REMOTE_W + BTN_CANCEL_W == W,
              "sync-conflict footer must span exactly W");

// Two-column layout for the comparison area.
constexpr int COL_GAP   = 12;
constexpr int COL_W     = (W - MARGIN_X * 2 - COL_GAP) / 2;
constexpr int COL_LEFT_X  = MARGIN_X;
constexpr int COL_RIGHT_X = MARGIN_X + COL_W + COL_GAP;
constexpr int COL_TOP   = BODY_TOP + 40;   // leave room for an intro line
constexpr int COL_H     = FOOTER_TOP - COL_TOP - MARGIN_Y;

// Row layout inside each column: heading + 5 data rows.
constexpr int COL_HEADING_OFFSET = 14;
constexpr int ROW_H              = 56;
constexpr int ROW_LABEL_DY       = 18;   // label baseline below row top
constexpr int ROW_VALUE_DY       = 40;   // value baseline below row top

// ─── Module state ──────────────────────────────────────────────────

SyncResult g_data{};
bool       g_pending = false;

// ─── Helpers ───────────────────────────────────────────────────────

// Format a Unix timestamp using project convention:
//   * "HH:MM"            when the timestamp falls on the device's local
//                        calendar day,
//   * "YYYY-MM-DD HH:MM" otherwise.
// Falls back to "—" when the timestamp is zero (server never assigned one,
// or the field was not populated by the transport client on pull).
void format_timestamp(uint32_t ts, char* out, size_t outLen) {
    if (outLen == 0) return;
    out[0] = '\0';
    if (ts == 0) {
        snprintf(out, outLen, "—");
        return;
    }

    const time_t t   = static_cast<time_t>(ts);
    const time_t now = time(nullptr);

    struct tm tmStamp{};
    struct tm tmNow{};
    if (!localtime_r(&t, &tmStamp)) {
        snprintf(out, outLen, "—");
        return;
    }
    const bool haveNow = (now > 0) && (localtime_r(&now, &tmNow) != nullptr);

    const bool sameDay = haveNow &&
                         tmStamp.tm_year == tmNow.tm_year &&
                         tmStamp.tm_yday == tmNow.tm_yday;

    if (sameDay) {
        strftime(out, outLen, "%H:%M", &tmStamp);
    } else {
        strftime(out, outLen, "%Y-%m-%d %H:%M", &tmStamp);
    }
}

// Render a centered label inside a button cell.
void draw_button(int x, int w, const char* label, uint8_t fg) {
    const int tw = display_text_width(label);
    const int tx = x + (w - tw) / 2;
    const int ty = FOOTER_TOP + FOOTER_HEIGHT - 14;
    display_draw_text(tx, ty, label, fg);
}

// Draw label/value pair at (x, y). Label is rendered in mid-gray, value in
// black so the eye lands on the numbers first.
void draw_label_value(int colX, int y, const char* label, const char* value) {
    display_draw_text(colX + 8, y + ROW_LABEL_DY, label, 6);
    // Truncate value from the right so it fits inside the column.
    const int maxW = COL_W - 16;
    String v(value);
    if (display_text_width(v.c_str()) > maxW) {
        while (v.length() > 1 &&
               display_text_width((v + "…").c_str()) > maxW) {
            v.remove(v.length() - 1);
        }
        v += "…";
    }
    display_draw_text(colX + 8, y + ROW_VALUE_DY, v.c_str(), 0);
}

// Render one full column (heading + five data rows) for the given
// KosyncProgress snapshot.
void draw_column(int colX, const char* heading, const KosyncProgress& p) {
    // Column frame
    display_draw_rect(colX, COL_TOP, COL_W, COL_H, 0);

    // Column heading band
    display_draw_filled_rect(colX + 1, COL_TOP + 1, COL_W - 2, 32, 13);
    const int hw = display_text_width(heading);
    display_draw_text(colX + (COL_W - hw) / 2,
                      COL_TOP + COL_HEADING_OFFSET + 10,
                      heading, 0);
    display_draw_hline(colX, COL_TOP + 33, COL_W, 0);

    int y = COL_TOP + 38;

    // Internal Paperloom convention is 0-based chapter/page (see SyncResult
    // and the rest of the reader). Surface that to users as "Kap. N" with
    // a "+1" so the dialog matches the on-screen page numbering, which is
    // 1-based by the time it reaches the user.
    char buf[48];

    snprintf(buf, sizeof(buf), "Kap. %d", p.chapter + 1);
    draw_label_value(colX, y, "Kapitel", buf);
    y += ROW_H;

    snprintf(buf, sizeof(buf), "Seite %d", p.page + 1);
    draw_label_value(colX, y, "Seite", buf);
    y += ROW_H;

    // Percentage with one decimal place.
    float pct = p.percentage * 100.0f;
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    snprintf(buf, sizeof(buf), "%.1f %%", pct);
    draw_label_value(colX, y, "Fortschritt", buf);
    y += ROW_H;

    const char* dev = p.device.c_str();
    if (!dev || dev[0] == '\0') dev = "—";
    draw_label_value(colX, y, "Gerät", dev);
    y += ROW_H;

    char tsBuf[24];
    format_timestamp(p.timestamp, tsBuf, sizeof(tsBuf));
    draw_label_value(colX, y, "Zeit", tsBuf);
}

}  // namespace

// ─── Public API ────────────────────────────────────────────────────

void ui_sync_conflict_set_data(const SyncResult& r) {
    g_data    = r;
    g_pending = true;
}

void ui_sync_conflict_draw() {
    display_set_font_size(2);
    display_fill_screen(15);
    drawHeader("Sync-Konflikt");

    // Intro line
    const char* intro =
        "Lokaler und Remote-Fortschritt unterscheiden sich.";
    const int iw = display_text_width(intro);
    display_draw_text((W - iw) / 2,
                      BODY_TOP + display_font_height() + 4,
                      intro, 0);

    // Two columns
    draw_column(COL_LEFT_X,  "Lokal",  g_data.local);
    draw_column(COL_RIGHT_X, "Remote", g_data.remote);

    // Footer button bar (dark band with three centred labels).
    display_draw_filled_rect(0, FOOTER_TOP, W, FOOTER_HEIGHT, 2);
    int fx = 0;
    draw_button(fx, BTN_LOCAL_W,  "Lokal behalten", 15);
    fx += BTN_LOCAL_W;
    display_draw_filled_rect(fx - 1, FOOTER_TOP + 4, 2, FOOTER_HEIGHT - 8, 10);
    draw_button(fx, BTN_REMOTE_W, "Remote übernehmen", 15);
    fx += BTN_REMOTE_W;
    display_draw_filled_rect(fx - 1, FOOTER_TOP + 4, 2, FOOTER_HEIGHT - 8, 10);
    draw_button(fx, BTN_CANCEL_W, "Abbrechen", 15);

    display_begin_frame();
    display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
    display_flush();
}

AppState ui_sync_conflict_touch(int x, int y) {
    // Safety net: if no SyncResult was staged, drop back to the reader
    // without touching the coordinator. This branch should never trigger
    // in a well-formed dispatch sequence.
    if (!g_pending) {
        return STATE_READER;
    }

    // Defense-in-depth: if the coordinator was somehow torn down between
    // entering this state and the user's tap, surface a generic error and
    // bail. The reader's busy flag (if any) is the coordinator's problem.
    if (!kosync_is_coordinator_initialized()) {
        Serial.println("[sync-conflict] coordinator missing on touch");
        ui_toast_show("Sync error", 2500, true);
        g_pending = false;
        return STATE_READER;
    }

    // Only the bottom button bar reacts. Taps elsewhere keep the dialog up.
    if (y < FOOTER_TOP) {
        return STATE_SYNC_CONFLICT;
    }

    auto& coord = kosync_get_coordinator();

    // [Lokal behalten]
    if (x < BTN_LOCAL_W) {
        Serial.println("[sync-conflict] resolve: keep local");
        SyncResult res = coord.resolveConflict(true);
        ui_toast_show(res.toast, 2500, !res.success);
        g_pending = false;
        return STATE_READER;
    }

    // [Remote übernehmen]
    if (x < BTN_LOCAL_W + BTN_REMOTE_W) {
        Serial.println("[sync-conflict] resolve: take remote");
        SyncResult res = coord.resolveConflict(false);
        ui_toast_show(res.toast, 2500, !res.success);
        g_pending = false;
        return STATE_READER;
    }

    // [Abbrechen]
    Serial.println("[sync-conflict] resolve: cancel");
    coord.clearBusy();
    ui_toast_show("Sync abgebrochen", 2000, false);
    g_pending = false;
    return STATE_READER;
}

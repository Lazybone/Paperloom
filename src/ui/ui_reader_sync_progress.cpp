// ui_reader_sync_progress.cpp — KoSync Progress-Screen (WP-10).
//
// Zeichnet eine 5-Punkt-Phasen-Liste + Statuszeile + Abbrechen-Button.
// Mapping von 9 SyncPhase-Werten auf 5 sichtbare Listeneintraege:
//
//   Liste                | SyncPhase(n)
//   ─────────────────────┼────────────────────────────────────────
//   1 WLAN verbinden     | WaitingWifi
//   2 Buch-Hash          | Hashing (Hashing kommt VOR WaitingWifi
//                        |   in der Sequenz; wir markieren sie
//                        |   beim Eintritt in WaitingWifi als done)
//   3 Server abrufen     | Pulling
//   4 Vergleich          | (Uebergang Pulling→Pushing) + AwaitConflict
//   5 Fertigstellen      | Pushing + Done

#include "ui_reader_sync_progress.h"

#include "ui_common.h"
#include "ui_toast.h"
#include "config.h"
#include "../display.h"
#include "../kosync_sync.h"

#include <Arduino.h>

namespace {

constexpr int W          = PORTRAIT_W;
constexpr int H          = PORTRAIT_H;
constexpr int BODY_TOP   = HEADER_HEIGHT + MARGIN_Y;
constexpr int FOOTER_TOP = H - FOOTER_HEIGHT;

constexpr int ROW_COUNT      = 5;
constexpr int ROW_HEIGHT     = 80;
constexpr int ROW_LIST_TOP   = BODY_TOP + 30;
constexpr int ICON_SIZE      = 28;
constexpr int ICON_LEFT      = MARGIN_X + 10;
constexpr int LABEL_LEFT     = ICON_LEFT + ICON_SIZE + 20;
constexpr int DETAIL_DY      = 30;  // detail line offset below label
constexpr int STATUS_LINE_Y  = FOOTER_TOP - 28;

constexpr uint8_t COLOR_FRAME_PENDING = 8;   // grey rect frame
constexpr uint8_t COLOR_BOX_FILLED    = 0;   // black for active/done/failed
constexpr uint8_t COLOR_TEXT_LABEL    = 0;
constexpr uint8_t COLOR_TEXT_DETAIL   = 6;
constexpr uint8_t COLOR_GLYPH_WHITE   = 15;

// Cancel-Button-Tap-Feedback (gedunkelte Box, bevor Cancel respektiert
// wird). Lebt nur waehrend eines einzelnen touch-Aufrufs.
bool s_cancelLatched = false;

enum class RowStatus : uint8_t { Pending, Active, Done, Failed };

struct RowSpec {
    const char* label;
};

const RowSpec kRows[ROW_COUNT] = {
    { "WLAN verbinden" },
    { "Buch-Hash"      },
    { "Server abrufen" },
    { "Vergleich"      },
    { "Fertigstellen"  },
};

// Phasen-Mapping: gibt fuer jede Zeile den Status zurueck.
RowStatus row_status_for(int rowIdx, SyncPhase ph) {
    auto rank = [](SyncPhase p) -> int {
        // Reihenfolge in der Sequenz; Done liegt hinter Pushing.
        switch (p) {
            case SyncPhase::Idle:          return -1;
            case SyncPhase::Hashing:       return 0;   // Zeile 2 active
            case SyncPhase::WaitingWifi:   return 1;   // Zeile 1 active
            case SyncPhase::Pulling:       return 2;
            case SyncPhase::AwaitConflict: return 3;   // Zeile 4 active
            case SyncPhase::Pushing:       return 4;
            case SyncPhase::Done:          return 5;
            case SyncPhase::Failed:        return -2;
            case SyncPhase::Cancelled:     return -3;
        }
        return -1;
    };

    const int r = rank(ph);
    if (ph == SyncPhase::Failed) {
        // Markiere die zuletzt active Zeile als failed; alles davor done.
        // Welche das ist, koennen wir aus dem pendingResult ableiten —
        // ohne diese Info markieren wir konservativ Zeile 5 als failed.
        if (rowIdx <= 1) return RowStatus::Done;
        if (rowIdx == 4) return RowStatus::Failed;
        return RowStatus::Pending;
    }
    if (ph == SyncPhase::Cancelled || ph == SyncPhase::Idle) {
        return RowStatus::Pending;
    }
    if (r < 0) return RowStatus::Pending;
    if (r >= 5) return RowStatus::Done;

    // Mapping rank → row-index der aktiven Zeile:
    static constexpr int kActiveRowForRank[5] = { 1, 0, 2, 3, 4 };
    const int activeRow = kActiveRowForRank[r];

    if (rowIdx <  activeRow) return RowStatus::Done;
    if (rowIdx == activeRow) return RowStatus::Active;
    return RowStatus::Pending;
}

// Zeichne ein 28x28 Phasen-Icon an (x,y).
void draw_icon(int x, int y, RowStatus st) {
    switch (st) {
        case RowStatus::Pending:
            display_draw_rect(x, y, ICON_SIZE, ICON_SIZE, COLOR_FRAME_PENDING);
            break;
        case RowStatus::Active:
            display_draw_filled_rect(x, y, ICON_SIZE, ICON_SIZE, COLOR_BOX_FILLED);
            // kleines weisses Quadrat in der Mitte als Hinweis "in Arbeit"
            display_draw_filled_rect(x + 10, y + 10, 8, 8, COLOR_GLYPH_WHITE);
            break;
        case RowStatus::Done:
            display_draw_filled_rect(x, y, ICON_SIZE, ICON_SIZE, COLOR_BOX_FILLED);
            // Stair-step Haekchen aus 4 weissen Mini-Quadraten (kein
            // line-Primitive verfuegbar; das ist die billigste Variante).
            display_draw_filled_rect(x +  6, y + 14, 4, 4, COLOR_GLYPH_WHITE);
            display_draw_filled_rect(x + 10, y + 18, 4, 4, COLOR_GLYPH_WHITE);
            display_draw_filled_rect(x + 14, y + 14, 4, 4, COLOR_GLYPH_WHITE);
            display_draw_filled_rect(x + 18, y + 10, 4, 4, COLOR_GLYPH_WHITE);
            break;
        case RowStatus::Failed:
            display_draw_filled_rect(x, y, ICON_SIZE, ICON_SIZE, COLOR_BOX_FILLED);
            // X aus 4 Diagonal-Pixeln pro Schenkel.
            for (int i = 0; i < 4; ++i) {
                display_draw_filled_rect(x +  8 + i*3, y +  8 + i*3, 3, 3, COLOR_GLYPH_WHITE);
                display_draw_filled_rect(x + 17 - i*3, y +  8 + i*3, 3, 3, COLOR_GLYPH_WHITE);
            }
            break;
    }
}

// Statuszeilen-Text — kurze deutsche Beschreibung der aktuellen Phase.
const char* status_text_for(SyncPhase ph) {
    switch (ph) {
        case SyncPhase::Idle:          return "Bereit";
        case SyncPhase::Hashing:       return "Berechne Buch-Hash …";
        case SyncPhase::WaitingWifi:   return "Verbinde mit WLAN …";
        case SyncPhase::Pulling:       return "Hole Server-Fortschritt …";
        case SyncPhase::AwaitConflict: return "Konflikt erkannt — bitte waehlen.";
        case SyncPhase::Pushing:       return "Sende Fortschritt zum Server …";
        case SyncPhase::Done:          return "Sync abgeschlossen.";
        case SyncPhase::Failed:        return "Sync fehlgeschlagen.";
        case SyncPhase::Cancelled:     return "Sync abgebrochen.";
    }
    return "";
}

}  // namespace

void ui_sync_progress_draw() {
    display_set_font_size(2);
    display_fill_screen(15);
    drawHeader("Sync Fortschritt");

    SyncPhase ph = kosync_is_coordinator_initialized()
                       ? kosync_get_coordinator().currentPhase()
                       : SyncPhase::Idle;

    // 5 Phasen-Zeilen
    for (int i = 0; i < ROW_COUNT; ++i) {
        const int rowY    = ROW_LIST_TOP + i * ROW_HEIGHT;
        const int iconY   = rowY + (ROW_HEIGHT - ICON_SIZE) / 2 - 8;
        const RowStatus st = row_status_for(i, ph);

        draw_icon(ICON_LEFT, iconY, st);
        display_draw_text(LABEL_LEFT, rowY + 36,
                          kRows[i].label, COLOR_TEXT_LABEL);
    }

    // Statuszeile
    const char* status = status_text_for(ph);
    const int sw = display_text_width(status);
    display_draw_text((W - sw) / 2, STATUS_LINE_Y, status, COLOR_TEXT_DETAIL);

    // Footer-Button "Abbrechen"
    display_draw_filled_rect(0, FOOTER_TOP, W, FOOTER_HEIGHT,
                             s_cancelLatched ? 4 : 2);
    const char* btn = "Abbrechen";
    const int bw = display_text_width(btn);
    display_draw_text((W - bw) / 2, FOOTER_TOP + FOOTER_HEIGHT - 14, btn,
                      COLOR_GLYPH_WHITE);

    display_begin_frame();
    display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
    display_flush();

    s_cancelLatched = false;
}

AppState ui_sync_progress_touch(int x, int y) {
    // Cancel-Button: gesamte Footer-Bar reagiert.
    if (y >= FOOTER_TOP) {
        if (kosync_is_coordinator_initialized()) {
            // Sofortiges visuelles Tap-Feedback: einmal mit gedunkeltem
            // Button neu zeichnen, dann Cancel anfordern. Der naechste
            // tick() im Dispatcher respektiert das Flag.
            s_cancelLatched = true;
            ui_sync_progress_draw();
            kosync_get_coordinator().requestCancel();
        }
        return STATE_SYNC_PROGRESS;
    }
    return STATE_SYNC_PROGRESS;
}

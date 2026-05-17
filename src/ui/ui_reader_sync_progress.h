#pragma once

// ui_reader_sync_progress.h — KoSync Progress-Screen (WP-10).
//
// Wird angezeigt, waehrend STATE_SYNC_PROGRESS aktiv ist. Liest die
// aktuelle SyncPhase vom KosyncSyncCoordinator und rendert sie als
// 5-Punkt-Liste mit Phasen-Icons (pending / active / done / failed)
// plus eine dynamische Statuszeile. Footer: einzelner [Abbrechen]-
// Button.
//
// State flow:
//   main.cpp dispatcher transitioniert nach STATE_SYNC_PROGRESS sobald
//   coord.beginSync() erfolgreich war. Anschliessend wird pro Frame
//   coord.tick() gerufen (im Dispatcher, NICHT in den UI-Funktionen
//   hier) und bei Phasenwechsel ui_sync_progress_draw() erneut
//   aufgerufen. Touch wird nur fuer den Abbrechen-Button ausgewertet.

#include "../../include/state.h"

void     ui_sync_progress_draw();
AppState ui_sync_progress_touch(int x, int y);

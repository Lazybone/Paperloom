#pragma once
#include "../../include/state.h"
#include "../kosync_sync.h"   // for SyncResult

// Conflict resolution dialog (WP-9).
//
// When syncNow() returns a SyncResult with hasConflict=true, the
// coordinator leaves its busy flag set and the main dispatcher transitions
// to STATE_SYNC_CONFLICT. The originating SyncResult must be passed in via
// ui_sync_conflict_set_data() before the first draw so the dialog can show
// local vs. remote progress without re-querying.
//
// On user choice the dialog routes the decision back into the coordinator:
//   * "Lokal behalten"    → resolveConflict(true)
//   * "Remote übernehmen" → resolveConflict(false)
//   * "Abbrechen"         → clearBusy()   (no I/O, just releases the flag)
// In every terminal case a toast is shown and STATE_READER is returned.

// Set by main.cpp dispatcher before transitioning to STATE_SYNC_CONFLICT.
// Holds the SyncResult from the originating syncNow() call so the dialog
// can read local/remote progress without re-querying.
void ui_sync_conflict_set_data(const SyncResult& r);

// Called by main.cpp draw dispatcher for STATE_SYNC_CONFLICT.
void ui_sync_conflict_draw();

// Called by main.cpp touch dispatcher. Returns the new AppState
// (STATE_SYNC_CONFLICT while waiting for user decision, or STATE_READER
// after Lokal behalten / Remote übernehmen / Abbrechen).
AppState ui_sync_conflict_touch(int x, int y);

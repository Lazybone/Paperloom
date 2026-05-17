#pragma once
#include <Arduino.h>
#include <atomic>
#include <memory>
#include "kosync_client.h"

// Coupling rule: KosyncSyncCoordinator holds a non-owning reference to BookReader.
// BookReader MUST NEVER hold a reference back to KosyncSyncCoordinator.
// If sync-aware behavior is needed in reader, introduce a SyncStateListener interface.

class BookReader;

enum class SyncPhase : uint8_t {
    Idle,
    Hashing,
    WaitingWifi,
    Pulling,
    AwaitConflict,
    Pushing,
    Done,
    Failed,
    Cancelled
};

struct SyncResult {
    bool success;
    String toast;
    bool hasConflict;
    KosyncProgress local;
    KosyncProgress remote;   // valid only when hasConflict
};

class KosyncSyncCoordinator {
public:
    explicit KosyncSyncCoordinator(BookReader& reader);

    // Initiate sync: builds client per-call from current Settings, pulls,
    // checks divergence. Returns SyncResult with hasConflict=true on conflict
    // (leaves busy flag set; caller must call resolveConflict or clearBusy).
    SyncResult syncNow();

    // Called after user picks in conflict dialog.
    // keepLocal=true  → PUSH local progress unchanged.
    // keepLocal=false → applyRemoteProgress on BookReader, then PUSH the new local.
    SyncResult resolveConflict(bool keepLocal);

    // Called from conflict dialog cancel path. Releases busy flag, no I/O.
    void clearBusy();

    bool isBusy() const { return busy_.load(); }

private:
    BookReader& reader_;
    std::atomic<bool> busy_{false};
    KosyncProgress pendingRemote_{};   // populated when entering AwaitConflict
    KosyncProgress pendingLocal_{};    // snapshot at sync-trigger time
};

// Lazy-init accessors (file-scope storage in kosync_sync.cpp).
// Must be called exactly once from setup() after settings_init() and BookReader is ready.
KosyncSyncCoordinator& kosync_initialize_coordinator(BookReader& reader);

// Asserts the coordinator was initialized; panics otherwise.
KosyncSyncCoordinator& kosync_get_coordinator();

// Cheap guard for callers that may run before setup() finishes (e.g. HTTP handlers).
bool kosync_is_coordinator_initialized();

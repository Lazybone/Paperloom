#pragma once
#include <Arduino.h>
#include <atomic>
#include <memory>
#include "kosync_client.h"

// Coupling rule: KosyncSyncCoordinator holds a non-owning reference to BookReader.
// BookReader MUST NEVER hold a reference back to KosyncSyncCoordinator.
// If sync-aware behavior is needed in reader, introduce a SyncStateListener interface.

class BookReader;

// Defined in kosync_sync.cpp (not in an anonymous namespace so the
// forward declaration here is well-formed under C++ ODR).
class WifiSyncGuard;

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

    // Out-of-line destructor required because unique_ptr<WifiSyncGuard> uses a
    // forward-declared type — the complete type must be visible at destruction site.
    ~KosyncSyncCoordinator();

    // ─── Conflict resolution (unveraendert) ────────────────────────────
    SyncResult resolveConflict(bool keepLocal);

    // Called from conflict dialog cancel path. Releases busy flag, no I/O.
    void       clearBusy();

    // True if the most recent restoreAfterSync attempt failed. Dispatcher
    // checks this and routes to STATE_LIBRARY with a toast.
    bool restoreFailed() const { return restoreFailed_; }
    void clearRestoreFailed() { restoreFailed_ = false; }

    bool       isBusy() const { return busy_.load(); }

    // ─── Phase-based API (WP-10) ───────────────────────────────────────

    // Startet eine neue Sync-Sequenz. Returns false wenn schon busy;
    // outToast wird dann mit dem User-sichtbaren Grund befuellt
    // ("Sync laeuft bereits"). Beim Erfolg ist phase_ == Hashing und
    // der Dispatcher muss state auf STATE_SYNC_PROGRESS setzen.
    bool beginSync(String& outToast);

    // Pro UI-Frame aufgerufen. Treibt die Phasen-Sequenz voran.
    // Idempotent in terminalen Phasen (Done, Failed, Cancelled,
    // AwaitConflict): tick() darf dort beliebig oft erneut gerufen
    // werden, ohne Seiteneffekte.
    // Returns true wenn sich die Phase in diesem Aufruf geaendert hat
    // (Dispatcher nutzt das als Redraw-Hint).
    bool tick();

    SyncPhase  currentPhase() const { return phase_; }

    // Returns the phase that was active when the sync transitioned to
    // Failed. Idle if no failure has been recorded since the last beginSync.
    SyncPhase lastFailedPhase() const { return failedAtPhase_; }

    // Gueltig wenn phase_ ∈ {Done, Failed, Cancelled, AwaitConflict}.
    // Liest pendingResult_ aus und setzt phase_ auf Idle zurueck —
    // genau einmal pro Sync-Sequenz aufrufen.
    SyncResult takeResult();

    // Atomar gesetzt; vom naechsten tick() respektiert.
    void requestCancel() { cancelRequested_.store(true); }

    // Synchroner Teardown vor enterDeepSleep(). Idempotent.
    void cancelIfBusy();

private:
    // ─── Internal helpers (Task 3) ─────────────────────────────────────
    void enterPhase(SyncPhase next);
    void runHashing();
    void runWaitingWifi();
    void runPulling();
    void runPushing();
    void finishWithToast(const String& toast, bool success);
    void finishConflict();
    void tryRestoreReader_();

    BookReader&                    reader_;
    std::atomic<bool>              busy_{false};
    std::atomic<bool>              cancelRequested_{false};
    SyncPhase                      phase_          = SyncPhase::Idle;
    SyncPhase                      lastPhase_      = SyncPhase::Idle;  // fuer tick()-changed-Hint
    SyncPhase                      failedAtPhase_  = SyncPhase::Idle;
    KosyncProgress                 pendingLocal_{};
    KosyncProgress                 pendingRemote_{};
    String                         hash_;
    SyncResult                     pendingResult_{};
    uint32_t                       wifiBudgetStartMs_ = 0;
    bool                           freshSync_         = false;  // 404-Pfad merken
    bool                           restoreFailed_     = false;
    std::unique_ptr<WifiSyncGuard> wifi_;
    std::unique_ptr<KosyncClient>  client_;
};

// Lazy-init accessors (file-scope storage in kosync_sync.cpp).
// Must be called exactly once from setup() after settings_init() and BookReader is ready.
KosyncSyncCoordinator& kosync_initialize_coordinator(BookReader& reader);

// Asserts the coordinator was initialized; panics otherwise.
KosyncSyncCoordinator& kosync_get_coordinator();

// Cheap guard for callers that may run before setup() finishes (e.g. HTTP handlers).
bool kosync_is_coordinator_initialized();

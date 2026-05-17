#include "kosync_sync.h"

#include <Arduino.h>
#include <WiFi.h>
#include <assert.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <memory>
#include <stdlib.h>
#include <time.h>

#include "kosync_client.h"
#include "reader.h"
#include "settings.h"

// ─── WifiSyncGuard ──────────────────────────────────────────────────────
//
// Stateful poll-based WiFi guard. Replaces the old blocking RAII guard.
//
// Usage:
//   WifiSyncGuard wifi(s);
//   auto br = wifi.begin();          // non-blocking start
//   while (...) { auto pr = wifi.poll(); ... }  // called once per tick
//   wifi.release();                  // explicit teardown (destructor is fallback)
//
// If WiFi is already connected (e.g. user is in WiFi-Upload screen), begin()
// returns AlreadyConnected and release() is a no-op (we don't own the radio).
//
// NOTE: NOT in an anonymous namespace so the forward declaration in the
// header resolves to the same type (C++ ODR requires identical linkage).
class WifiSyncGuard {
public:
    enum class BeginResult { Started, NoCredentials, AlreadyConnected, WifiInitFailed };
    enum class PollResult  { WaitingShort, Connected, Failed };

    explicit WifiSyncGuard(const Settings& s) : ssid_(s.wifiSSID), pass_(s.wifiPass) {}

    ~WifiSyncGuard() { release(); }

    WifiSyncGuard(const WifiSyncGuard&)            = delete;
    WifiSyncGuard& operator=(const WifiSyncGuard&) = delete;

    // Non-blocking start. Sets internal state so subsequent poll() can be called.
    BeginResult begin() {
        if (WiFi.status() == WL_CONNECTED) {
            return BeginResult::AlreadyConnected;
        }
        if (ssid_.length() == 0) {
            return BeginResult::NoCredentials;
        }
        Serial.printf("[kosync_sync] heap before WiFi.begin: dma_free=%u dma_largest=%u\n",
                      static_cast<unsigned>(heap_caps_get_free_size(
                          MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(
                          MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)));
        WiFi.mode(WIFI_STA);
        wl_status_t st = WiFi.begin(ssid_.c_str(), pass_.c_str());
        if (st == WL_CONNECT_FAILED) {
            // esp_wifi_init or STA-enable failed (typically DMA-cap RAM
            // exhausted — see esp_wifi log lines above). No point waiting
            // 10s for a poll() that will never reach Connected.
            Serial.println("[kosync_sync] WiFi.begin returned WL_CONNECT_FAILED");
            return BeginResult::WifiInitFailed;
        }
        startMs_     = millis();
        weBroughtUp_ = true;     // tentativ; release() berücksichtigt es
        return BeginResult::Started;
    }

    // Called once per UI tick while in WaitingWifi phase. Non-blocking.
    PollResult poll() {
        wl_status_t st = WiFi.status();

        // Diagnostik fuer DMA-Pressure-Hypothese (siehe historisches Log).
        // Nur in den ersten ~500 ms loggen — kein Spam ueber das gesamte
        // 10-s-Budget.
        uint32_t age = millis() - startMs_;
        if (age <= 500 && (age / 50) != lastLoggedSlot_) {
            lastLoggedSlot_ = age / 50;
            Serial.printf("[kosync_sync] heap t=%lums: dma_largest=%u status=%d\n",
                          age,
                          static_cast<unsigned>(heap_caps_get_largest_free_block(
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
                          static_cast<int>(st));
        }

        if (st == WL_CONNECTED) {
            return PollResult::Connected;
        }
        if (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED) {
            return PollResult::Failed;
        }
        // WL_DISCONNECTED / WL_IDLE_STATUS / WL_SCAN_COMPLETED: weiter warten.
        return PollResult::WaitingShort;
    }

    // Tear WiFi back down — only if we were the ones that brought it up.
    // Idempotent.
    void release() {
        if (released_) return;
        if (weBroughtUp_) {
            // Tear down regardless of current WL state: begin() may have been
            // called but never reached Connected, OR we did connect and now
            // it's time to release the radio. Both paths need disconnect+OFF.
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
        }
        released_ = true;
    }

    bool weBroughtUp() const { return weBroughtUp_ && !released_; }

private:
    String   ssid_;
    String   pass_;
    uint32_t startMs_        = 0;
    uint32_t lastLoggedSlot_ = UINT32_MAX;
    bool     weBroughtUp_    = false;
    bool     released_       = false;
};

// ─── File-scope storage for lazy-init accessors ─────────────────────────
namespace {

std::unique_ptr<KosyncSyncCoordinator> g_coordinator;

// Divergence thresholds. Local-vs-remote progress is considered convergent
// (no conflict dialog) when chapter matches AND the page delta is within
// kPageDeltaThreshold AND percentage delta is within kPercentageDeltaThreshold.
constexpr int   kPageDeltaThreshold = 2;
constexpr float kPercentageDeltaThreshold = 0.01f;   // 1%

// Defense-in-depth bounds on the server-returned timestamp. We allow up to
// 5 minutes of forward clock skew and refuse anything older than 5 years.
constexpr uint32_t kTimestampToleranceSec = 300;                  // 5 min future skew
constexpr uint32_t kTimestampMaxAgeSec    = 5u * 365u * 24u * 3600u;  // 5 years past

// Bounds-check a freshly-pulled remote KosyncProgress. chapter/page are not
// validated here because KoReader does not emit them — BookReader::applyRemoteProgress
// re-validates page bounds at apply time (it needs the loaded chapter's page count).
bool bounds_ok(const KosyncProgress& p) {
    if (p.percentage < 0.0f || p.percentage > 1.0f) return false;
    const time_t nowSec = time(nullptr);
    // If the device clock is unset (epoch < 2001), skip the timestamp
    // window check — we can't reason about freshness without a real clock.
    if (nowSec > 978307200) {  // 2001-01-01
        const uint32_t now = static_cast<uint32_t>(nowSec);
        if (p.timestamp > now + kTimestampToleranceSec) return false;
        if (p.timestamp != 0 && p.timestamp + kTimestampMaxAgeSec < now) return false;
    }
    return true;
}

// Encode local progress as Paperloom-native "c=<chapter>/<totalChapters> p=<page>/<totalPages>".
// Round-trippable by parse_paperloom_progress() below. KoReader cannot interpret
// this; for cross-client sync we fall back to percentage-only divergence checks.
String reader_local_progress_string(const BookReader& r) {
    String s;
    s.reserve(48);
    s += "c=";
    s += String(r.getCurrentChapter());
    s += "/";
    s += String(r.getTotalChapters());
    s += " p=";
    s += String(r.getCurrentPage());
    s += "/";
    s += String(r.getTotalPages());
    return s;
}

// Approximate fraction-through-book. Mirrors getApproxBookPercent() but returns
// 0..1 instead of 0..100. Best-effort: returns 0.0f if the book has no measurable
// extent yet (per concept §10, percentage is informational on the wire).
float reader_compute_percentage(const BookReader& r) {
    const int total = r.getApproxBookPageCount();
    if (total <= 1) return 0.0f;
    const int pos = r.getApproxBookPage();
    float pct = static_cast<float>(pos) / static_cast<float>(total - 1);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    return pct;
}

// Best-effort parser for the Paperloom-emitted "c=<c>/<tot> p=<p>/<tot>" format.
// On parse failure, outputs are left unchanged so callers can fall back to
// chapter=local.chapter and percentage-only divergence.
void parse_paperloom_progress(const String& s, int& outChapter, int& outPage) {
    if (s.length() == 0) return;
    int cIdx = s.indexOf("c=");
    int pIdx = s.indexOf("p=");
    if (cIdx < 0 || pIdx < 0) return;

    // "c=<digits>" → up to '/' or ' '
    int cEnd = cIdx + 2;
    while (cEnd < (int)s.length() && isDigit(s[cEnd])) cEnd++;
    if (cEnd == cIdx + 2) return;
    int parsedC = s.substring(cIdx + 2, cEnd).toInt();

    int pEnd = pIdx + 2;
    while (pEnd < (int)s.length() && isDigit(s[pEnd])) pEnd++;
    if (pEnd == pIdx + 2) return;
    int parsedP = s.substring(pIdx + 2, pEnd).toInt();

    outChapter = parsedC;
    outPage = parsedP;
}

// Snapshot current reader state into a KosyncProgress ready for PUSH.
void snapshot_local(const BookReader& r, const Settings& s, KosyncProgress& out) {
    out.device     = s.kosyncDeviceName;
    out.deviceId   = s.kosyncDeviceName;   // reuse name as id (no separate id in Settings)
    out.progress   = reader_local_progress_string(r);
    out.chapter    = r.getCurrentChapter();
    out.page       = r.getCurrentPage();
    out.percentage = reader_compute_percentage(r);
    out.timestamp  = static_cast<uint32_t>(time(nullptr));
}

// Translate a PUSH HTTP status into a toast string. Returns true if success.
bool push_status_to_toast(int status, const String& err, String& outToast,
                          const char* successMsg) {
    if (status == 200 || status == 201) {
        outToast = successMsg;
        return true;
    }
    if (status == 401 || status == 403) {
        outToast = "Sync fehlgeschlagen: Login ungültig";
    } else if (status == 0) {
        outToast = err.length()
                       ? String("Sync fehlgeschlagen: ") + err
                       : String("Sync fehlgeschlagen: Server nicht erreichbar");
    } else {
        outToast = "Sync fehlgeschlagen: Serverfehler";
    }
    return false;
}

}  // namespace

// ─── Lazy-init accessors ────────────────────────────────────────────────

KosyncSyncCoordinator& kosync_initialize_coordinator(BookReader& reader) {
    if (!g_coordinator) {
        // Note: project defaults to C++11; std::make_unique requires C++14.
        g_coordinator.reset(new KosyncSyncCoordinator(reader));
    }
    return *g_coordinator;
}

KosyncSyncCoordinator& kosync_get_coordinator() {
    assert(g_coordinator && "kosync_initialize_coordinator() not called");
    return *g_coordinator;
}

bool kosync_is_coordinator_initialized() {
    return g_coordinator != nullptr;
}

// ─── KosyncSyncCoordinator ──────────────────────────────────────────────

KosyncSyncCoordinator::KosyncSyncCoordinator(BookReader& reader)
    : reader_(reader) {}

SyncResult KosyncSyncCoordinator::resolveConflict(bool keepLocal) {
    SyncResult r{};
    r.success = false;
    r.hasConflict = false;

    // Must be in conflict state (busy_ stays true through AwaitConflict phase).
    // If not, something is out of order — fail loudly but cheaply.
    if (!busy_.load()) {
        r.toast = "Sync läuft nicht";
        return r;
    }

    // Re-validate settings (user may have entered the settings screen between
    // conflict dialog open and resolution).
    const Settings& s = settings_get();
    if (s.kosyncServer.length() == 0 || s.kosyncUser.length() == 0 ||
        s.kosyncKey.length() != 32 || s.kosyncCredentialsInvalid) {
        Serial.printf("[kosync_sync] resolve: not configured\n");
        r.toast = "KoSync nicht konfiguriert";
        busy_.store(false);
        return r;
    }

    String hash = reader_.getDocumentHash();
    if (hash.length() != 32) {
        Serial.printf("[kosync_sync] resolve: bad doc hash\n");
        r.toast = "Sync fehlgeschlagen: Serverfehler";
        busy_.store(false);
        return r;
    }

    // Bring WiFi up only after cheap checks pass. The guard tears it down
    // on scope exit if we connected here.
    WifiSyncGuard wifi(s);
    auto br = wifi.begin();
    if (br == WifiSyncGuard::BeginResult::NoCredentials) {
        Serial.printf("[kosync_sync] resolve: no WiFi creds\n");
        r.toast = "WLAN nicht konfiguriert";
        busy_.store(false);
        return r;
    }
    if (br == WifiSyncGuard::BeginResult::WifiInitFailed) {
        Serial.printf("[kosync_sync] resolve: WiFi init failed\n");
        r.toast = "Sync fehlgeschlagen: WLAN-Stack nicht startbar";
        busy_.store(false);
        return r;
    }
    if (br == WifiSyncGuard::BeginResult::Started) {
        const uint32_t deadline = millis() + 10000;
        WifiSyncGuard::PollResult pr;
        do {
            pr = wifi.poll();
            if (pr == WifiSyncGuard::PollResult::Connected) break;
            if (pr == WifiSyncGuard::PollResult::Failed)    break;
            delay(50);
        } while (millis() < deadline);
        if (pr != WifiSyncGuard::PollResult::Connected) {
            Serial.printf("[kosync_sync] resolve: WiFi connect failed\n");
            r.toast = "Sync fehlgeschlagen: Kein WLAN";
            busy_.store(false);
            return r;
        }
    }
    // br == AlreadyConnected → fall through, no wait needed.

    KosyncClient client(s.kosyncServer, s.kosyncUser, s.kosyncKey);

    bool saveFailed = false;
    if (!keepLocal) {
        // Apply remote progress to the reader, then re-snapshot local.
        BookReader::ApplyResult ar = reader_.applyRemoteProgress(
            pendingRemote_.chapter, pendingRemote_.page, pendingRemote_.percentage);
        if (ar == BookReader::ApplyResult::OutOfBounds) {
            Serial.printf("[kosync_sync] resolve: applyRemote OutOfBounds\n");
            r.toast = "Sync fehlgeschlagen: Serverfehler";
            busy_.store(false);
            return r;
        }
        if (ar == BookReader::ApplyResult::SaveFailed) {
            // Continue to PUSH so the server still receives our agreement,
            // but flag the secondary warning for the success toast.
            saveFailed = true;
            Serial.printf("[kosync_sync] resolve: applyRemote SaveFailed (continuing)\n");
        }
        // Re-snapshot — local now reflects the applied remote position.
        snapshot_local(reader_, s, pendingLocal_);
    }

    String err;
    int ps = client.pushProgress(hash, pendingLocal_, err);

    const char* successMsg = keepLocal
                                 ? "Sync ok (Server aktualisiert)"
                                 : (saveFailed
                                        ? "Sync ok (Server) — Lokales Speichern fehlgeschlagen"
                                        : "Sync ok (Lokal aktualisiert)");
    if (push_status_to_toast(ps, err, r.toast, successMsg)) {
        reader_.setLastSyncTimestamp(static_cast<uint32_t>(time(nullptr)));
        r.success = true;
        r.local = pendingLocal_;
    } else {
        Serial.printf("[kosync_sync] resolve: push failed status=%d\n", ps);
    }
    busy_.store(false);
    // WP-10: release the WifiSyncGuard that finishConflict() kept alive
    // for the resolve-push round-trip. The member must be torn down here
    // so DMA-capable heap is freed immediately for the next page-paint.
    if (wifi_) wifi_->release();
    wifi_.reset();
    return r;
}

void KosyncSyncCoordinator::clearBusy() {
    busy_.store(false);
}

// ─── WP-10 phase-based API ──────────────────────────────────────────────

KosyncSyncCoordinator::~KosyncSyncCoordinator() = default;

void KosyncSyncCoordinator::enterPhase(SyncPhase next) {
    phase_ = next;
}

bool KosyncSyncCoordinator::beginSync(String& outToast) {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) {
        outToast = "Sync läuft bereits";
        return false;
    }
    cancelRequested_.store(false);
    pendingLocal_      = KosyncProgress{};
    pendingRemote_     = KosyncProgress{};
    pendingResult_     = SyncResult{};
    hash_              = String();
    freshSync_         = false;
    wifiBudgetStartMs_ = 0;
    failedAtPhase_     = SyncPhase::Idle;
    wifi_.reset();
    client_.reset();
    enterPhase(SyncPhase::Hashing);
    lastPhase_ = SyncPhase::Idle;   // erster tick() registriert Phasenwechsel
    return true;
}

bool KosyncSyncCoordinator::tick() {
    // Cancel hat hoechste Prioritaet — terminal stoppen, falls in einer
    // unterbrechbaren Phase.
    if (cancelRequested_.load() &&
        phase_ != SyncPhase::AwaitConflict &&
        phase_ != SyncPhase::Done &&
        phase_ != SyncPhase::Failed &&
        phase_ != SyncPhase::Cancelled) {
        Serial.printf("[kosync_sync] sync: cancelled in phase=%d\n",
                      static_cast<int>(phase_));
        pendingResult_.success     = false;
        pendingResult_.hasConflict = false;
        pendingResult_.toast       = "Sync abgebrochen";
        if (wifi_) wifi_->release();
        client_.reset();
        busy_.store(false);
        enterPhase(SyncPhase::Cancelled);
    } else {
        switch (phase_) {
            case SyncPhase::Idle:          break;
            case SyncPhase::Hashing:       runHashing();      break;
            case SyncPhase::WaitingWifi:   runWaitingWifi();  break;
            case SyncPhase::Pulling:       runPulling();      break;
            case SyncPhase::Pushing:       runPushing();      break;
            case SyncPhase::AwaitConflict: break;
            case SyncPhase::Done:          break;
            case SyncPhase::Failed:        break;
            case SyncPhase::Cancelled:     break;
        }
    }

    const bool changed = (phase_ != lastPhase_);
    lastPhase_ = phase_;
    return changed;
}

void KosyncSyncCoordinator::runHashing() {
    hash_ = reader_.getDocumentHash();
    if (hash_.length() != 32) {
        Serial.printf("[kosync_sync] sync: bad doc hash (len=%u)\n",
                      static_cast<unsigned>(hash_.length()));
        finishWithToast("Sync fehlgeschlagen: Serverfehler", false);
        return;
    }
    const Settings& s = settings_get();
    if (s.kosyncServer.length() == 0 || s.kosyncUser.length() == 0 ||
        s.kosyncKey.length() != 32 || s.kosyncCredentialsInvalid) {
        Serial.printf("[kosync_sync] sync: not configured\n");
        finishWithToast("KoSync nicht konfiguriert", false);
        return;
    }
    wifi_.reset(new WifiSyncGuard(s));
    auto br = wifi_->begin();
    if (br == WifiSyncGuard::BeginResult::NoCredentials) {
        Serial.printf("[kosync_sync] sync: no WiFi creds\n");
        finishWithToast("WLAN nicht konfiguriert", false);
        return;
    }
    if (br == WifiSyncGuard::BeginResult::WifiInitFailed) {
        Serial.printf("[kosync_sync] sync: WiFi init failed (DMA-heap pressure?)\n");
        finishWithToast("Sync fehlgeschlagen: WLAN-Stack nicht startbar", false);
        return;
    }
    wifiBudgetStartMs_ = millis();
    enterPhase(SyncPhase::WaitingWifi);
}

void KosyncSyncCoordinator::runWaitingWifi() {
    auto pr = wifi_->poll();
    if (pr == WifiSyncGuard::PollResult::Connected) {
        const Settings& s = settings_get();
        client_.reset(new KosyncClient(s.kosyncServer, s.kosyncUser, s.kosyncKey));
        snapshot_local(reader_, s, pendingLocal_);
        enterPhase(SyncPhase::Pulling);
        return;
    }
    if (pr == WifiSyncGuard::PollResult::Failed) {
        Serial.printf("[kosync_sync] sync: WiFi connect failed\n");
        finishWithToast("Sync fehlgeschlagen: Kein WLAN", false);
        return;
    }
    if (millis() - wifiBudgetStartMs_ >= 10000) {
        Serial.printf("[kosync_sync] sync: WiFi budget expired\n");
        finishWithToast("Sync fehlgeschlagen: Kein WLAN", false);
        return;
    }
    // sonst: in WaitingWifi bleiben, nächster tick versucht erneut
}

void KosyncSyncCoordinator::runPulling() {
    String err;
    int status = client_->pullProgress(hash_, pendingRemote_, err);

    if (status == 404) {
        freshSync_ = true;
        enterPhase(SyncPhase::Pushing);
        return;
    }
    if (status == 401 || status == 403) {
        Serial.printf("[kosync_sync] sync: pull auth failed status=%d\n", status);
        finishWithToast("Sync fehlgeschlagen: Login ungültig", false);
        return;
    }
    if (status == 0) {
        Serial.printf("[kosync_sync] sync: pull transport error\n");
        String toast = err.length()
                           ? String("Sync fehlgeschlagen: ") + err
                           : String("Sync fehlgeschlagen: Server nicht erreichbar");
        finishWithToast(toast, false);
        return;
    }
    if (status != 200) {
        Serial.printf("[kosync_sync] sync: pull server error status=%d\n", status);
        finishWithToast("Sync fehlgeschlagen: Serverfehler", false);
        return;
    }
    if (!bounds_ok(pendingRemote_)) {
        Serial.printf("[kosync_sync] OutOfBounds: percentage=%.4f, timestamp=%u\n",
                      pendingRemote_.percentage, pendingRemote_.timestamp);
        finishWithToast("Sync fehlgeschlagen: Serverfehler", false);
        return;
    }
    pendingRemote_.chapter = pendingLocal_.chapter;
    pendingRemote_.page    = pendingLocal_.page;
    parse_paperloom_progress(pendingRemote_.progress,
                             pendingRemote_.chapter, pendingRemote_.page);

    const bool divergent =
        (pendingLocal_.chapter != pendingRemote_.chapter) ||
        (abs(pendingLocal_.page - pendingRemote_.page) > kPageDeltaThreshold) ||
        (fabsf(pendingLocal_.percentage - pendingRemote_.percentage) >
         kPercentageDeltaThreshold);

    if (divergent) {
        finishConflict();
        return;
    }
    enterPhase(SyncPhase::Pushing);
}

void KosyncSyncCoordinator::runPushing() {
    String err;
    int ps = client_->pushProgress(hash_, pendingLocal_, err);
    const char* successMsg = freshSync_ ? "Sync ok (neu)" : "Sync ok";
    String toast;
    bool ok = push_status_to_toast(ps, err, toast, successMsg);
    if (ok) {
        reader_.setLastSyncTimestamp(static_cast<uint32_t>(time(nullptr)));
        pendingResult_.local = pendingLocal_;
    } else {
        Serial.printf("[kosync_sync] sync: push failed status=%d\n", ps);
    }
    finishWithToast(toast, ok);
}

void KosyncSyncCoordinator::finishWithToast(const String& toast, bool success) {
    if (!success) {
        failedAtPhase_ = phase_;   // remember which phase actually failed
    }
    pendingResult_.success     = success;
    pendingResult_.hasConflict = false;
    pendingResult_.toast       = toast;
    if (success) pendingResult_.local = pendingLocal_;
    if (wifi_) wifi_->release();
    client_.reset();
    busy_.store(false);
    enterPhase(success ? SyncPhase::Done : SyncPhase::Failed);
}

void KosyncSyncCoordinator::finishConflict() {
    pendingResult_.success     = false;
    pendingResult_.hasConflict = true;
    pendingResult_.toast       = "";
    pendingResult_.local       = pendingLocal_;
    pendingResult_.remote      = pendingRemote_;
    // wifi_ + busy_ bleiben aktiv — resolveConflict() reused beide.
    enterPhase(SyncPhase::AwaitConflict);
}

SyncResult KosyncSyncCoordinator::takeResult() {
    SyncResult r = pendingResult_;
    pendingResult_ = SyncResult{};
    enterPhase(SyncPhase::Idle);
    lastPhase_ = SyncPhase::Idle;
    return r;
}

void KosyncSyncCoordinator::cancelIfBusy() {
    if (!busy_.load()) return;
    Serial.printf("[kosync_sync] cancelIfBusy: phase=%d\n",
                  static_cast<int>(phase_));
    cancelRequested_.store(true);
    if (wifi_) wifi_->release();
    client_.reset();
    busy_.store(false);
    enterPhase(SyncPhase::Idle);
    lastPhase_ = SyncPhase::Idle;
}

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

// ─── File-scope storage for lazy-init accessors ─────────────────────────
namespace {

std::unique_ptr<KosyncSyncCoordinator> g_coordinator;

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
class WifiSyncGuard {
public:
    enum class BeginResult { Started, NoCredentials, AlreadyConnected };
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
        WiFi.begin(ssid_.c_str(), pass_.c_str());
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
        if (weBroughtUp_ && WiFi.status() != WL_CONNECTED) {
            // begin() wurde gerufen, aber wir haben nie Connected gesehen
            // (oder die Verbindung ist abgerissen). WiFi-Stack trotzdem
            // sauber abreissen.
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
        } else if (weBroughtUp_) {
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

SyncResult KosyncSyncCoordinator::syncNow() {
    SyncResult r{};
    r.success = false;
    r.hasConflict = false;

    // Busy guard (atomic CAS — safe against concurrent UI taps).
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) {
        r.toast = "Sync läuft bereits";
        return r;
    }

    // 1. Document hash (local — no WiFi needed)
    String hash = reader_.getDocumentHash();
    if (hash.length() != 32) {
        Serial.printf("[kosync_sync] sync: bad doc hash (len=%u)\n",
                      static_cast<unsigned>(hash.length()));
        r.toast = "Sync fehlgeschlagen: Serverfehler";
        busy_.store(false);
        return r;
    }

    // 2. Validate settings + build client per-call (L1 contract: no shared state)
    const Settings& s = settings_get();
    if (s.kosyncServer.length() == 0 || s.kosyncUser.length() == 0 ||
        s.kosyncKey.length() != 32 || s.kosyncCredentialsInvalid) {
        Serial.printf("[kosync_sync] sync: not configured\n");
        r.toast = "KoSync nicht konfiguriert";
        busy_.store(false);
        return r;
    }

    // 3. Bring WiFi up for the duration of this call. The guard tears it
    //    back down on scope exit if we were the ones who connected.
    //
    // Transitional: this synchronous wait stays here until syncNow() is
    // removed in Task 8. Mirrors the old behavior using the new begin/poll
    // API so the WifiSyncGuard refactor lands compile-clean.
    WifiSyncGuard wifi(s);
    auto br = wifi.begin();
    if (br == WifiSyncGuard::BeginResult::NoCredentials) {
        Serial.printf("[kosync_sync] sync: no WiFi creds\n");
        r.toast = "WLAN nicht konfiguriert";
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
            Serial.printf("[kosync_sync] sync: WiFi connect failed\n");
            r.toast = "Sync fehlgeschlagen: Kein WLAN";
            busy_.store(false);
            return r;
        }
    }
    // br == AlreadyConnected → fall through, no wait needed.

    KosyncClient client(s.kosyncServer, s.kosyncUser, s.kosyncKey);

    // 4. Snapshot local progress
    snapshot_local(reader_, s, pendingLocal_);

    // 5. PULL
    String err;
    pendingRemote_ = KosyncProgress{};   // reset before reuse
    int status = client.pullProgress(hash, pendingRemote_, err);

    // 6. Status routing
    if (status == 404) {
        // Fresh sync — no remote yet, go straight to PUSH.
        int ps = client.pushProgress(hash, pendingLocal_, err);
        if (push_status_to_toast(ps, err, r.toast, "Sync ok (neu)")) {
            reader_.setLastSyncTimestamp(static_cast<uint32_t>(time(nullptr)));
            r.success = true;
            r.local = pendingLocal_;
        } else {
            Serial.printf("[kosync_sync] sync: fresh-push failed status=%d\n", ps);
        }
        busy_.store(false);
        return r;
    }

    if (status == 401 || status == 403) {
        Serial.printf("[kosync_sync] sync: pull auth failed status=%d\n", status);
        r.toast = "Sync fehlgeschlagen: Login ungültig";
        busy_.store(false);
        return r;
    }
    if (status == 0) {
        Serial.printf("[kosync_sync] sync: pull transport error\n");
        r.toast = err.length()
                      ? String("Sync fehlgeschlagen: ") + err
                      : String("Sync fehlgeschlagen: Server nicht erreichbar");
        busy_.store(false);
        return r;
    }
    if (status != 200) {
        Serial.printf("[kosync_sync] sync: pull server error status=%d\n", status);
        r.toast = "Sync fehlgeschlagen: Serverfehler";
        busy_.store(false);
        return r;
    }

    // 7. Defense-in-depth bounds check on remote payload.
    if (!bounds_ok(pendingRemote_)) {
        Serial.printf("[kosync_sync] OutOfBounds: percentage=%.4f, timestamp=%u\n",
                      pendingRemote_.percentage, pendingRemote_.timestamp);
        r.toast = "Sync fehlgeschlagen: Serverfehler";
        busy_.store(false);
        return r;
    }

    // 8. Try to derive chapter/page from the remote progress string. KoReader
    //    emits XPath-style markers we can't interpret; if parse fails, default
    //    to local.chapter so divergence is judged on percentage only.
    pendingRemote_.chapter = pendingLocal_.chapter;
    pendingRemote_.page    = pendingLocal_.page;
    parse_paperloom_progress(pendingRemote_.progress,
                             pendingRemote_.chapter, pendingRemote_.page);

    // 9. Divergence check
    const bool divergent =
        (pendingLocal_.chapter != pendingRemote_.chapter) ||
        (abs(pendingLocal_.page - pendingRemote_.page) > kPageDeltaThreshold) ||
        (fabsf(pendingLocal_.percentage - pendingRemote_.percentage) >
         kPercentageDeltaThreshold);

    if (divergent) {
        r.hasConflict = true;
        r.local  = pendingLocal_;
        r.remote = pendingRemote_;
        r.toast  = "";   // dialog will set toast on resolution
        // Leave busy_ true — coordinator waits for resolveConflict() / clearBusy().
        return r;
    }

    // 10. Convergent — PUSH and finish.
    int ps = client.pushProgress(hash, pendingLocal_, err);
    if (push_status_to_toast(ps, err, r.toast, "Sync ok")) {
        reader_.setLastSyncTimestamp(static_cast<uint32_t>(time(nullptr)));
        r.success = true;
        r.local = pendingLocal_;
    } else {
        Serial.printf("[kosync_sync] sync: convergent-push failed status=%d\n", ps);
    }
    busy_.store(false);
    return r;
}

SyncResult KosyncSyncCoordinator::resolveConflict(bool keepLocal) {
    SyncResult r{};
    r.success = false;
    r.hasConflict = false;

    // Must be in conflict state (busy_ stays true through syncNow's conflict
    // return). If not, something is out of order — fail loudly but cheaply.
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
    //
    // Transitional: same synchronous wait as syncNow() — mirrors old behavior
    // using the new begin/poll API so the WifiSyncGuard refactor lands
    // compile-clean.
    WifiSyncGuard wifi(s);
    auto br = wifi.begin();
    if (br == WifiSyncGuard::BeginResult::NoCredentials) {
        Serial.printf("[kosync_sync] resolve: no WiFi creds\n");
        r.toast = "WLAN nicht konfiguriert";
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
    return r;
}

void KosyncSyncCoordinator::clearBusy() {
    busy_.store(false);
}

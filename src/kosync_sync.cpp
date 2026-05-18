#include "kosync_sync.h"

#include <Arduino.h>
#include <WiFi.h>
#include <assert.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
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
            //
            // CRITICAL: we already called WiFi.mode(WIFI_STA) which
            // allocates internal WiFi stack memory. Without an explicit
            // WIFI_OFF here, that allocation accumulates across failed
            // attempts and eventually starves the DMA heap completely
            // (observed: dma_largest 28KB → 16 bytes after 1-2 retries).
            Serial.println("[kosync_sync] WiFi.begin returned WL_CONNECT_FAILED");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
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
            // Stop SNTP first so the background task doesn't try to refresh
            // mid-teardown and hold its socket. Safe even if SNTP was never
            // started (esp_sntp_stop is a no-op then).
            esp_sntp_stop();

            // Tear down regardless of current WL state: begin() may have been
            // called but never reached Connected, OR we did connect and now
            // it's time to release the radio. Both paths need disconnect+OFF.
            // disconnect(wifioff=true, eraseAP=false) — eraseAP would wipe
            // the saved SSID/password from NVS, which we DO want to keep.
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);

            // Give the WiFi driver + lwIP tasks one scheduler tick to
            // actually return their internal buffers to the heap. Without
            // this delay the next state's free-heap snapshot underreads
            // because WiFi-tear-down is asynchronous.
            delay(120);

            Serial.printf("[wifi] post-release heap: free=%u "
                          "dma_largest=%u\n",
                          static_cast<unsigned>(ESP.getFreeHeap()),
                          static_cast<unsigned>(heap_caps_get_largest_free_block(
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)));
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

// Kick off an SNTP query once per boot. Idempotent: subsequent calls are
// no-ops. Non-blocking — esp32-arduino's configTzTime() spawns the SNTP
// task in the background and returns immediately. By the time the sync
// reaches push (~1-3 seconds after the first call), time(nullptr) usually
// returns a real epoch; if it doesn't, snapshot_local guards with 0u so
// we never push seconds-since-boot as a timestamp.
//
// TZ: Germany (CET/CEST). format_timestamp() in the conflict UI uses
// localtime_r() and will render correct local time once SNTP completes.
void ntp_kickoff_once() {
    static bool fired = false;
    if (fired) return;
    fired = true;
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3",
                 "pool.ntp.org", "time.nist.gov");
    Serial.printf("[kosync_sync] NTP kickoff (pool.ntp.org, TZ CET/CEST)\n");
}

// Defense-in-depth ceiling on book/page indices parsed from a server
// response. No real EPUB has six-digit chapter or page counts; values
// beyond this are treated as malformed and rejected silently (the caller
// keeps its pendingLocal_ fallback). Shared across parser + any future
// validator so both sides agree on the same bound.
constexpr int kMaxBookIndex = 100000;

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

// Average characters per Paperloom line on the rendered page. The reader
// strips XML/HTML markup down to plain text and breaks into lines around
// ~60 chars (varies with font/margin settings, but the page-break logic
// targets visual fit, not exact character count). 60 is the midpoint we
// use for the lossy XPath text-node-estimate below; both push and pull
// use the same value AND the same linesPerPage so Paperloom→Paperloom
// sync is self-consistent and cross-client sync lands within ~one
// Paperloom page of the original position.
constexpr int kApproxCharsPerLine = 60;

// Defensive clamp on the per-chapter page index before it feeds the
// text-node-estimate multiplication. `getCurrentPage()` is bounded by
// `_totalPages - 1` at all known call sites, but a future refactor
// breaking that invariant would cause `page * linesPerPage *
// kApproxCharsPerLine` to overflow int32 silently and emit garbage to
// the server. The cap below keeps the product safely below INT_MAX even
// at the largest linesPerPage (~40) and kApproxCharsPerLine (60):
//   1,000,000 * 40 * 60 = 2.4e9 — still inside int32_t (max 2.147e9), so
// we cap at 800,000 to leave headroom for future spacing tuning.
constexpr int kMaxPageForTextEstimate = 800000;

// Build a KOReader-canonical XPath progress string for the current reader
// position. Format:
//   "/body/DocFragment[<chapter+1>]/body/text().<textNodeEstimate>"
// where:
//   * DocFragment[N] is the 1-indexed spine position (Paperloom stores
//     chapter 0-indexed; KOReader/CRengine expect 1-indexed).
//   * text().<N> in KOReader's CRengine is technically a text-node child
//     index, NOT a character offset. We use it as a lossy chapter-
//     position hint: clients pulling from us land somewhere in the same
//     chapter (KOReader/Readest don't strictly round-trip text().N
//     either when the inner element path is missing). The encoded value
//     is `page * linesPerPage * kApproxCharsPerLine`, which other
//     clients interpret as "approximately this many text nodes in" — in
//     practice close enough for resume-reading UX, and the `percentage`
//     field carries finer signal anyway.
//
// `linesPerPage` is taken from the caller because the reader's layout
// metric varies with fontSizeLevel/lineSpacing/orientation. Passing it
// explicitly (rather than reading from r inside) lets the pull side use
// the SAME value that was captured at push time, so round-trips work
// even after layout settings change between push and pull.
String reader_koreader_xpath(const BookReader& r, int linesPerPage) {
    const int displayChapter = r.getCurrentChapter() + 1;
    int page = r.getCurrentPage();
    if (page < 0) page = 0;
    if (page > kMaxPageForTextEstimate) page = kMaxPageForTextEstimate;
    if (linesPerPage <= 0) linesPerPage = 1;
    // Defensive upper bound on linesPerPage. Today's layouts top out at
    // ~40 lines/page (small font, tight spacing); cap at 200 to catch any
    // future layout regression that would otherwise overflow the int
    // product below. 800000 * 200 * 60 = 9.6e9, well above INT_MAX, so we
    // also cap the product itself rather than trusting the multiplicands.
    if (linesPerPage > 200) linesPerPage = 200;

    int64_t product = static_cast<int64_t>(page) *
                      static_cast<int64_t>(linesPerPage) *
                      static_cast<int64_t>(kApproxCharsPerLine);
    if (product > 1'900'000'000) product = 1'900'000'000;
    const int textNodeEstimate = static_cast<int>(product);

    // Worst-case length: "/body/DocFragment[" (18) + 6-digit chapter (6)
    // + "]/body/text()." (14) + 10-digit offset (10) + NUL = 49. Reserve
    // 64 with headroom; static_assert prevents silent reallocation if
    // any of these bounds drift in future edits.
    static_assert(18 + 6 + 14 + 10 + 1 <= 64,
                  "reader_koreader_xpath reserve must fit the worst-case "
                  "encoded length without forcing String reallocation");
    String s;
    s.reserve(64);
    s += "/body/DocFragment[";
    s += String(displayChapter);
    s += "]/body/text().";
    s += String(textNodeEstimate);
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

// Best-effort parser for the KOReader-native XPath progress format. Other
// clients (KOReader, CrossInk, Readest) emit positions like
//   /body/DocFragment[12]/body/div[1]/text().42
// where DocFragment[N] is the 1-indexed chapter (matches Paperloom's display
// convention `chapter + 1`) and the optional trailing `text().<N>` is a
// character-offset hint. Returns true when a chapter was successfully
// extracted; sets outTextOffset to the parsed integer when a text().N is
// present, otherwise -1 (no hint).
bool parse_koreader_xpath_progress(const String& s, int& outChapter,
                                   int& outTextOffset) {
    outTextOffset = -1;
    if (s.length() == 0 || s.length() > 128) return false;
    const int marker = s.indexOf("DocFragment[");
    if (marker < 0) return false;
    const int numStart = marker + 12;  // length of "DocFragment["
    int numEnd = numStart;
    while (numEnd < (int)s.length() && isDigit(s[numEnd])) numEnd++;
    if (numEnd == numStart) return false;
    if (numEnd >= (int)s.length() || s[numEnd] != ']') return false;
    // Cap digit count BEFORE toInt() to avoid atoi-style overflow UB on
    // a hostile server returning "DocFragment[99999999999...]" within the
    // 128-char input cap. 7 digits is well above kMaxBookIndex (100000)
    // and safely below INT_MAX (10 digits).
    if (numEnd - numStart > 7) return false;
    const int parsed = s.substring(numStart, numEnd).toInt();
    // DocFragment is 1-indexed; convert to Paperloom's 0-indexed internal
    // chapter. Defense-in-depth against absurd values from a hostile or
    // buggy server.
    const int internalChapter = parsed - 1;
    if (internalChapter < 0 || internalChapter > kMaxBookIndex) {
        Serial.printf("[kosync_sync] parse_koreader_xpath_progress: rejecting "
                      "out-of-range DocFragment index (%d)\n", parsed);
        return false;
    }
    outChapter = internalChapter;

    // Optional `text().<N>` suffix — character-offset hint within the
    // chapter. Used by Paperloom→Paperloom round-trip (we emit the same
    // estimate on push) and to land approximately at the right spot when
    // pulling from clients that include it.
    const int textMarker = s.indexOf("text().", numEnd);
    if (textMarker >= 0) {
        const int offStart = textMarker + 7;  // length of "text()."
        int offEnd = offStart;
        while (offEnd < (int)s.length() && isDigit(s[offEnd])) offEnd++;
        // Cap digit count BEFORE toInt() to avoid atoi-style overflow UB
        // (same reasoning as DocFragment[N] above). 9 digits covers the
        // post-parse bound kMaxBookIndex * 1000 = 100,000,000 (9 digits)
        // safely below INT_MAX (10 digits).
        if (offEnd > offStart && (offEnd - offStart) <= 9) {
            const int parsedOffset = s.substring(offStart, offEnd).toInt();
            if (parsedOffset >= 0 && parsedOffset <= kMaxBookIndex * 1000) {
                outTextOffset = parsedOffset;
            }
        }
    }
    return true;
}

// Estimate Paperloom-internal page from a KOReader XPath text().N value.
// Mirrors the encoding in reader_koreader_xpath(): we treat the value as
// approximately `page * linesPerPage * kApproxCharsPerLine`. The estimate
// clamps to [0, totalChapterPages - 1]. Returns -1 if no useful estimate
// can be derived (no text-offset, or invalid layout metrics).
//
// `linesPerPage` and `totalChapterPages` come from the caller's snapshot
// (captured before releaseForSync) so the estimate still works during
// runPulling() after the reader has been released. Cross-client pulls
// will use the local snapshot values as a best-effort proxy — the result
// is bounded to the local chapter's page count, with the actual page
// re-clamped by applyRemoteProgress() once the remote chapter is loaded.
int estimate_page_from_text_offset(int textOffset, int linesPerPage,
                                   int totalChapterPages) {
    if (textOffset < 0 || totalChapterPages <= 0 || linesPerPage <= 0) {
        return -1;
    }
    const int charsPerPage = linesPerPage * kApproxCharsPerLine;
    int est = textOffset / charsPerPage;
    if (est < 0) est = 0;
    if (est > totalChapterPages - 1) est = totalChapterPages - 1;
    return est;
}

// Best-effort parser for the LEGACY Paperloom-emitted "c=<c>/<tot> p=<p>/<tot>"
// format. Retained for backward-compat with records on the kosync server
// that older Paperloom firmware wrote before we switched to canonical
// KOReader XPath emission. On parse failure, outputs are left unchanged
// so callers can fall back to chapter=local.chapter and percentage-only
// divergence.
void parse_paperloom_progress(const String& s, int& outChapter, int& outPage) {
    if (s.length() == 0) return;
    // Reject anything that looks like a KOReader XPath — the substring
    // `p=` could otherwise match inside a hypothetical future XPath
    // (e.g. `path=` parameters) and would silently extract garbage
    // values. A real Paperloom legacy record never contains DocFragment.
    if (s.indexOf("DocFragment") >= 0) return;
    // Hard length cap — Paperloom-emitted format is "c=N/M p=N/M" with
    // realistic upper bound ~30 chars. A server returning anything wildly
    // longer is either malformed or hostile (or just a KoReader native
    // progress string we don't understand); refuse to scan it. Log it so
    // diagnostics can distinguish "length-rejected" from "no c=/p=
    // markers" (KoReader native format silently falls through below).
    if (s.length() > 128) {
        Serial.printf("[kosync_sync] parse_paperloom_progress: rejecting "
                      "oversized string (len=%u)\n",
                      static_cast<unsigned>(s.length()));
        return;
    }
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

    // Defense-in-depth against absurd values from a hostile or buggy
    // server. kMaxBookIndex is declared in the anonymous namespace at the
    // top of this file so any future emitter/validator stays in sync.
    if (parsedC < 0 || parsedC > kMaxBookIndex ||
        parsedP < 0 || parsedP > kMaxBookIndex) {
        Serial.printf("[kosync_sync] parse_paperloom_progress: rejecting "
                      "out-of-range values (c=%d p=%d)\n",
                      parsedC, parsedP);
        return;
    }

    outChapter = parsedC;
    outPage = parsedP;
}

// Snapshot current reader state into a KosyncProgress ready for PUSH.
//
// Push format is KOReader-canonical XPath so KOReader/CrossInk/Readest can
// interpret the chapter (and roughly locate within it via text-offset).
// Records written by older Paperloom firmware used a custom "c=N/M p=N/M"
// string; the pull-side parser handles both formats for backward compat,
// and the next push from any Paperloom client overwrites the legacy record
// on the server with the canonical XPath.
//
// `linesPerPage` is passed in so the caller can capture it BEFORE
// releaseForSync() closes the book and zeroes the reader's layout
// metrics — this is what makes Paperloom→Paperloom round-trips
// page-accurate across font/spacing settings.
void snapshot_local(const BookReader& r, const Settings& s, int linesPerPage,
                    KosyncProgress& out) {
    out.device     = s.kosyncDeviceName;
    out.deviceId   = s.kosyncDeviceName;   // reuse name as id (no separate id in Settings)
    out.progress   = reader_koreader_xpath(r, linesPerPage);
    out.chapter    = r.getCurrentChapter();
    out.page       = r.getCurrentPage();
    out.percentage = reader_compute_percentage(r);
    // Only attach a timestamp if the device clock is actually set. Without
    // NTP, time(nullptr) returns seconds-since-boot — pushing that to the
    // server pollutes the timestamp field and surfaces as "00:02" in the
    // conflict dialog. 0 here means "unknown"; the server assigns its own
    // on push and the conflict dialog renders "—" for ts==0.
    const time_t now = time(nullptr);
    out.timestamp = (now > 978307200)  // 2001-01-01
                        ? static_cast<uint32_t>(now)
                        : 0u;
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
        g_coordinator = std::make_unique<KosyncSyncCoordinator>(reader);
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
        pendingResult_ = r;
        // finishConflict() kept wifi_ alive for this resolve. Tear it
        // down on every exit path so the radio doesn't stay up.
        wifi_.reset();
        client_.reset();
        busy_.store(false);
        return r;
    }

    String hash = reader_.getDocumentHash();
    if (hash.length() != 32) {
        Serial.printf("[kosync_sync] resolve: bad doc hash\n");
        r.toast = "Sync fehlgeschlagen: Serverfehler";
        pendingResult_ = r;
        wifi_.reset();
        client_.reset();
        busy_.store(false);
        return r;
    }

    // WiFi was brought up during runHashing/runWaitingWifi and
    // finishConflict() intentionally kept the member wifi_ alive across the
    // AwaitConflict phase precisely so we DO NOT have to re-establish the
    // radio (and pay another handshake cost) here. Re-using a previously
    // started stack-local guard would have been redundant and a future
    // ownership hazard if anyone added weBroughtUp tracking to the
    // AlreadyConnected branch.
    //
    // The guard below verifies (a) the WifiSyncGuard object still owns its
    // teardown contract (wifi_ non-null) and (b) the radio is still
    // associated. It does NOT prove "we brought it up" — when the original
    // begin() returned AlreadyConnected, weBroughtUp_ is false and the
    // destructor's release() is a no-op, which is exactly what we want.
    // If the AP dropped mid-dialog, bail cleanly rather than retry (a
    // retry here would call WiFi.begin while the EPUB parser may still be
    // open from applyRemoteProgress below, risking DMA-heap OOM).
    if (!wifi_ || WiFi.status() != WL_CONNECTED) {
        Serial.printf("[kosync_sync] resolve: wifi member missing or "
                      "disconnected (status=%d)\n",
                      static_cast<int>(WiFi.status()));
        r.toast = "Sync fehlgeschlagen: Kein WLAN";
        pendingResult_ = r;
        wifi_.reset();
        client_.reset();
        busy_.store(false);
        return r;
    }

    KosyncClient client(s.kosyncServer, s.kosyncUser, s.kosyncKey);

    bool saveFailed = false;
    if (!keepLocal) {
        // Apply remote progress to the reader, then re-snapshot local.
        BookReader::ApplyResult ar = reader_.applyRemoteProgress(
            pendingRemote_.chapter, pendingRemote_.page, pendingRemote_.percentage);
        if (ar == BookReader::ApplyResult::OutOfBounds) {
            Serial.printf("[kosync_sync] resolve: applyRemote OutOfBounds\n");
            r.toast = "Sync fehlgeschlagen: Serverfehler";
            pendingResult_ = r;
            // releaseForSync() has NOT been called yet on this path, so the
            // reader is still open and no tryRestoreReader_() is needed.
            // If a future edit moves releaseForSync() above applyRemoteProgress,
            // this branch must add tryRestoreReader_() before returning.
            wifi_.reset();
            client_.reset();
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
        // Capture current layout metrics: applyRemoteProgress() loads the
        // remote chapter and re-paginates, so getTotalPages and
        // getLinesPerPage both reflect the post-apply layout.
        snapshotLinesPerPage_ = reader_.getLinesPerPage();
        snapshotTotalPages_   = reader_.getTotalPages();
        snapshot_local(reader_, s, snapshotLinesPerPage_, pendingLocal_);
    }

    // Plan H: release the BookReader before the TLS push handshake, then
    // restore afterwards. applyRemoteProgress (above, if !keepLocal) needs
    // the reader open, so the release must happen after that call.
    if (!reader_.releaseForSync()) {
        Serial.printf("[kosync_sync] resolve: releaseForSync failed (SD?)\n");
        r.toast   = "Sync fehlgeschlagen: SD nicht beschreibbar";
        r.success = false;
        // Also mirror into pendingResult_ so a later takeResult() cannot
        // expose the stale finishConflict() state (success=false, toast="")
        // and surface a blank-toast failure to the user.
        pendingResult_         = r;
        wifi_.reset();
        client_.reset();
        busy_.store(false);
        return r;
    }
    String err;
    int ps = client.pushProgress(hash, pendingLocal_, err);
    tryRestoreReader_();

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
    // wifi_.reset() invokes the destructor which calls release() — no need
    // to do it explicitly here.
    wifi_.reset();
    // client_ is already null here (finishConflict() cleared it before the
    // AwaitConflict phase; the push above used a stack-local KosyncClient).
    // Reset defensively to match every other exit path in this function
    // and to stay correct if the flow ever re-populates client_.
    client_.reset();
    return r;
}

void KosyncSyncCoordinator::clearBusy() {
    if (wifi_) wifi_->release();
    wifi_.reset();
    client_.reset();
    tryRestoreReader_();
    cancelRequested_.store(false);
    busy_.store(false);
    enterPhase(SyncPhase::Idle);
    lastPhase_ = SyncPhase::Idle;
}

// ─── WP-10 phase-based API ──────────────────────────────────────────────

KosyncSyncCoordinator::~KosyncSyncCoordinator() = default;

void KosyncSyncCoordinator::tryRestoreReader_() {
    if (!reader_.restoreAfterSync()) {
        Serial.printf("[kosync_sync] restoreAfterSync FAILED (SD removed? file deleted?)\n");
        restoreFailed_ = true;
    }
}

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
    restoreFailed_         = false;
    pendingLocal_          = KosyncProgress{};
    pendingRemote_         = KosyncProgress{};
    pendingResult_         = SyncResult{};
    hash_                  = String();
    freshSync_             = false;
    wifiBudgetStartMs_     = 0;
    failedAtPhase_         = SyncPhase::Idle;
    // Zero the layout-metric snapshot fields so a sync that gets
    // cancelled before runHashing() captures fresh values cannot leak
    // stale ones into estimate_page_from_text_offset (which would still
    // be caught by its <=0 guard, but the explicit reset removes the
    // hidden dependency).
    snapshotLinesPerPage_  = 0;
    snapshotTotalPages_    = 0;
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
        tryRestoreReader_();
        cancelRequested_.store(false);
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
    // CRITICAL: snapshot local progress BEFORE releaseForSync closes the book
    // and resets chapter/page to 0. Otherwise we'd push zeros to the server.
    // Same applies to linesPerPage/totalPages, which the pull-side
    // estimate_page_from_text_offset() needs even after release.
    snapshotLinesPerPage_ = reader_.getLinesPerPage();
    snapshotTotalPages_   = reader_.getTotalPages();
    snapshot_local(reader_, s, snapshotLinesPerPage_, pendingLocal_);

    // WP-10 Plan H: fully close the BookReader (parser + cached state) so
    // all reader DMA-cap RAM is freed before WiFi.begin. We restore in
    // finishWithToast()/finishConflict() before returning to the UI.
    Serial.printf("[kosync_sync] releasing BookReader before WiFi.begin\n");
    if (!reader_.releaseForSync()) {
        // saveProgress failed (SD missing/full/RO). Book stayed open;
        // no restore needed. Abort the sync cleanly.
        Serial.printf("[kosync_sync] sync: releaseForSync failed (SD?)\n");
        finishWithToast("Sync fehlgeschlagen: SD nicht beschreibbar", false);
        return;
    }

    wifi_ = std::make_unique<WifiSyncGuard>(s);
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
    if (br == WifiSyncGuard::BeginResult::AlreadyConnected) {
        // WiFi already up (e.g. WiFi-upload-page is active). Skip WaitingWifi.
        ntp_kickoff_once();
        client_ = std::make_unique<KosyncClient>(s.kosyncServer, s.kosyncUser, s.kosyncKey);
        enterPhase(SyncPhase::Pulling);
    } else {
        wifiBudgetStartMs_ = millis();
        enterPhase(SyncPhase::WaitingWifi);
    }
}

void KosyncSyncCoordinator::runWaitingWifi() {
    auto pr = wifi_->poll();
    if (pr == WifiSyncGuard::PollResult::Connected) {
        const Settings& s = settings_get();
        ntp_kickoff_once();
        client_ = std::make_unique<KosyncClient>(s.kosyncServer, s.kosyncUser, s.kosyncKey);
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
    // Diagnostic: surface what the server actually returned so cross-client
    // sync issues (e.g. CrossInk pushes KoReader native XPath) are debuggable
    // without having to instrument the HTTP client. `progress` is non-secret
    // book-internal locator data; safe to log.
    Serial.printf("[kosync_sync] pull: remote progress=\"%s\" pct=%.4f ts=%u dev=\"%s\"\n",
                  pendingRemote_.progress.c_str(),
                  pendingRemote_.percentage,
                  pendingRemote_.timestamp,
                  pendingRemote_.device.c_str());

    // Treat a fully-empty 200 response as a fresh sync. Some kosync servers
    // (notably koreader-sync-server variants) return HTTP 200 with an empty
    // JSON object when no record exists for the hash, instead of 404. Without
    // this guard we'd always trigger a conflict dialog against pct=0.0 even
    // though there is nothing to merge against.
    //
    // Invariants relied on:
    //   * Real records always carry a non-empty `progress` string.
    //     Paperloom (reader_koreader_xpath emits "/body/DocFragment[N]/..."
    //     unconditionally) and KOReader/CrossInk/Readest (same XPath
    //     format) all push non-empty progress, so progress.length()==0
    //     rules out any legitimate push-echo. Records from older
    //     Paperloom firmware used "c=N/M p=N/M" — also non-empty.
    //   * Real records also carry a non-empty device + deviceId. KOSync
    //     servers reject pushes without them; checking these too prevents
    //     a false fresh-sync if a server ever returned a corrupted record
    //     with progress stripped but device retained.
    //   * `timestamp == 0` is included for safety but not strictly required:
    //     Paperloom intentionally pushes ts=0 when the device clock is
    //     unset (see snapshot_local). If such a record were echoed back,
    //     the non-empty progress/device fields above would still keep the
    //     guard from firing.
    //   * percentage comes from `doc["percentage"] | 0.0f`, so a missing
    //     field is indistinguishable from a real 0.0. The combined check
    //     with progress + device + deviceId still uniquely identifies
    //     "server has no real record" without false positives.
    if (pendingRemote_.progress.length() == 0 &&
        pendingRemote_.percentage == 0.0f &&
        pendingRemote_.timestamp == 0u &&
        pendingRemote_.device.length() == 0 &&
        pendingRemote_.deviceId.length() == 0) {
        Serial.printf("[kosync_sync] pull: 200 with empty record — treating as fresh sync\n");
        freshSync_ = true;
        enterPhase(SyncPhase::Pushing);
        return;
    }

    // Seed remote chapter/page with local values as the fallback when no
    // parser succeeds. We try formats in priority order:
    //   1. KOReader XPath (canonical, what we push now and what other
    //      clients emit). Yields chapter + optional text-offset for page.
    //   2. Paperloom legacy "c=N/M p=N/M" (records from older firmware).
    // If XPath succeeded for chapter, we DO NOT fall through to the
    // legacy parser — a record can't be in both formats simultaneously,
    // and running the legacy parser on an XPath would corrupt the page.
    pendingRemote_.chapter = pendingLocal_.chapter;
    pendingRemote_.page    = pendingLocal_.page;

    int xpathChapter = pendingLocal_.chapter;
    int xpathTextOffset = -1;
    const bool gotXpath = parse_koreader_xpath_progress(
        pendingRemote_.progress, xpathChapter, xpathTextOffset);

    if (gotXpath) {
        pendingRemote_.chapter = xpathChapter;
        if (xpathChapter != pendingLocal_.chapter) {
            // Remote is in a different chapter than us. Local page index
            // is meaningless in the remote's chapter — start at page 0
            // and let the text-offset estimate refine if present.
            pendingRemote_.page = 0;
        }
        if (xpathTextOffset >= 0) {
            // Estimate a Paperloom page from the text-offset. The reader
            // is currently RELEASED (book closed before WiFi.begin), so
            // we use the layout metrics captured at snapshot time. This
            // is the local chapter's pagination, which is a useful proxy
            // for Paperloom→Paperloom sync where both sides have similar
            // chapter sizes. For cross-client pulls into a different
            // chapter, the estimate is bounded by the local chapter's
            // page count; applyRemoteProgress() re-clamps to the actual
            // remote-chapter page count once the chapter is loaded.
            const int est = estimate_page_from_text_offset(
                xpathTextOffset, snapshotLinesPerPage_,
                snapshotTotalPages_);
            if (est >= 0) pendingRemote_.page = est;
        }
        Serial.printf("[kosync_sync] pull: parsed XPath chapter=%d, "
                      "textOffset=%d → page=%d\n",
                      pendingRemote_.chapter, xpathTextOffset,
                      pendingRemote_.page);
    } else {
        // Try the legacy Paperloom format. parse_paperloom_progress leaves
        // outputs unchanged on miss, so the local-fallback seed survives.
        const int beforeChapter = pendingRemote_.chapter;
        const int beforePage    = pendingRemote_.page;
        parse_paperloom_progress(pendingRemote_.progress,
                                 pendingRemote_.chapter, pendingRemote_.page);
        if (pendingRemote_.chapter == beforeChapter &&
            pendingRemote_.page == beforePage &&
            pendingRemote_.progress.length() > 0) {
            // Unknown format. Log the fallback once so traces can tell
            // "non-divergent because remote matched" from "non-divergent
            // because we used local values".
            Serial.printf("[kosync_sync] pull: using local chapter/page fallback "
                          "(remote progress in unknown format)\n");
        }
    }

    // Note on the page-delta term: when XPath parsing yielded a chapter
    // match with no text-offset hint (the common case for clients that
    // emit bare DocFragment[N] paths), pendingRemote_.page is left at
    // pendingLocal_.page, so the page-delta evaluates to 0 and only the
    // percentage axis can trigger a conflict. This is intentional —
    // without a position hint we cannot decide whether the remote is at
    // a different page than us, and surfacing a spurious conflict every
    // sync would be worse UX than relying on the percentage check.
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
    Serial.printf("[kosync_sync] restoring BookReader after sync\n");
    tryRestoreReader_();
    enterPhase(success ? SyncPhase::Done : SyncPhase::Failed);
}

void KosyncSyncCoordinator::finishConflict() {
    pendingResult_.success     = false;
    pendingResult_.hasConflict = true;
    pendingResult_.toast       = "";
    pendingResult_.local       = pendingLocal_;
    pendingResult_.remote      = pendingRemote_;
    // wifi_ + busy_ bleiben aktiv — resolveConflict() reused beide.
    // Pull-phase client is no longer needed; resolveConflict() uses its own
    // stack-local KosyncClient.
    client_.reset();
    // BookReader must be open before AwaitConflict because the user's
    // resolveConflict choice may trigger applyRemoteProgress which
    // reads the EPUB.
    Serial.printf("[kosync_sync] restoring BookReader before conflict dialog\n");
    tryRestoreReader_();
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
    wifi_.reset();
    client_.reset();
    // restoreAfterSync is idempotent — only acts if the book was actually
    // released. finishConflict() already restores before AwaitConflict, so
    // calling here is safe.
    tryRestoreReader_();
    busy_.store(false);
    enterPhase(SyncPhase::Idle);
    lastPhase_ = SyncPhase::Idle;
}

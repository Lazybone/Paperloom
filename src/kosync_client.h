#pragma once
#include <Arduino.h>
#include <memory>
#include <WiFiClientSecure.h>

// KosyncProgress mirrors the kosync wire schema plus two Paperloom-internal
// fields (`chapter`, `page`) that the coordinator derives from `progress` and
// `percentage`. The transport client does NOT populate `chapter` / `page` on
// pull — they stay at 0 here and are interpreted by the caller.
struct KosyncProgress {
    String device;
    String deviceId;
    String progress;        // KoReader "progress" position-marker string
    int    chapter;         // 0-based Paperloom-internal
    int    page;            // 0-based Paperloom-internal
    float  percentage;      // 0.0..1.0
    uint32_t timestamp;     // Unix seconds (server-assigned on push)
};

// Thin HTTPS wrapper around the kosync REST API. One instance per pull/push
// call — the underlying WiFiClientSecure is lazily created and torn down
// inside each call, so no state leaks across operations. Credentials are
// validated in the constructor; if validation fails, both API calls
// short-circuit to status 0 with a redacted error log.
class KosyncClient {
public:
    KosyncClient(const String& serverUrl,
                 const String& username,
                 const String& passwordMd5);

    bool isWiFiConnected() const;

    // Returns HTTP status code:
    //   200 success — out_progress populated
    //   404 fresh-sync — out_progress unmodified, out_error empty
    //   401/403 auth — out_error populated with user-facing message
    //   5xx server — out_error populated
    //     0 network/TLS/DNS/timeout — out_error populated
    int pullProgress(const String& documentHash,
                     KosyncProgress& out_progress,
                     String& out_error);

    int pushProgress(const String& documentHash,
                     const KosyncProgress& progress,
                     String& out_error);

private:
    String server_;
    String user_;
    String key_;
    bool   invalid_ = false;  // set when serverUrl/keyMd5 fails validation; calls then short-circuit to status 0
    std::unique_ptr<WiFiClientSecure> client_;  // lazy; one per pull/push call (no across-call reuse)

    void ensureClient_();
    void resetClient_();
    bool validateDocHash_(const String& h) const;
};

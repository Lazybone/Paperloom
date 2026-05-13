// kosync_pin_state.h — PIN gate for web-UI credential writes (WP-6c).
//
// 6-digit PIN displayed on the e-paper as an out-of-band channel to gate
// credential-changing endpoints (POST /api/kosync-settings,
// POST /api/kosync-register). The PIN is generated on demand, expires after
// kPinExpiryMs, and accepts a single validation attempt before being
// consumed. After kPinLockoutThreshold consecutive mismatches the module
// enters a kPinLockoutDurationMs lockout window during which neither
// validation nor fresh-issue is allowed (HTTP layer returns 429 +
// Retry-After).
//
// Threading: single-threaded main-loop invariant. The
// g_kosyncShowPinRequested flag is std::atomic so it survives a future
// migration to AsyncWebServer / dual-core dispatch — main.cpp's draw
// dispatcher consumes the request with an exchange(false).
#pragma once

#include <Arduino.h>
#include <atomic>
#include <stdint.h>

// Result code returned by kosync_pin_validate().
enum class KosyncPinResult {
    Ok,          // valid PIN consumed; caller may proceed
    Mismatch,    // wrong PIN value (failCount incremented)
    Expired,     // active PIN expired before validation
    NoActive,    // no PIN currently issued
    RateLimited  // lockout window active; do NOT display a PIN
};

// Generate a fresh 6-digit PIN. Stores it in module state and requests the
// main loop to transition to STATE_KOSYNC_PIN_PROMPT via the atomic flag.
// Returns true if generated; false if lockout is currently active (caller
// must respond with 429 + Retry-After).
bool kosync_pin_generate();

// True iff a non-expired PIN exists with attempts remaining AND lockout is
// not active. Read-only — never mutates state.
bool kosync_pin_is_active();

// Validate a candidate PIN. Side effects per concept §4:
//   * Ok          — consumes PIN (zeroes value), resets failCount.
//   * Mismatch    — increments failCount; PIN remains active for retry.
//   * Expired     — clears stale PIN value; no failCount change.
//   * NoActive    — no PIN ever issued or already consumed; no change.
//   * RateLimited — lockout active; no change.
// After step Mismatch increments failCount, if failCount >=
// kPinLockoutThreshold the module enters lockout (invalidates active PIN,
// resets failCount) and returns RateLimited instead.
KosyncPinResult kosync_pin_validate(uint32_t candidate);

// Wipe PIN value via volatile-pointer+read-back, clear deferred-show flag,
// reset failCount and lockoutUntilMillis. Called on STATE_KOSYNC_SETUP
// entry (WP-7 dispatch) and on the cancel tap from the prompt screen.
void kosync_pin_reset_state();

// True iff currently in lockout. Optionally fills out_remaining_ms with
// milliseconds left in the lockout window. Wrap-safe arithmetic. As a
// side effect, clears the lockout if it has expired.
bool kosync_pin_is_locked_out(uint32_t* out_remaining_ms = nullptr);

// Used by main-loop dispatcher to decide whether to transition into
// STATE_KOSYNC_PIN_PROMPT. The flag is cleared atomically by exchange(false)
// on consumption (in main.cpp).
extern std::atomic<bool> g_kosyncShowPinRequested;

// PIN value accessor for the UI module. Returns the 6-digit value if
// active, or 0 if no active PIN (caller MUST check kosync_pin_is_active()
// first — the UI module zero-pads with %06u).
uint32_t kosync_pin_current_value();

// Milliseconds until the active PIN expires, for the UI countdown.
// Returns 0 if no active PIN.
uint32_t kosync_pin_remaining_ms();

// Tuning constants — exposed so the UI module and unit tests can reference
// the same source of truth.
static constexpr uint8_t  kPinLockoutThreshold  = 3;
static constexpr uint32_t kPinLockoutDurationMs = 5u * 60u * 1000u;  // 5 min
static constexpr uint32_t kPinExpiryMs          = 60u * 1000u;       // 60 s

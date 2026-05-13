// kosync_pin_state.cpp — PIN gate state machine (WP-6c).
//
// See kosync_pin_state.h for the public contract. Step ordering (per
// concept §4 / iter3 fixes) is enforced by the HTTP layer; this module
// implements the underlying state transitions.

#include "kosync_pin_state.h"

#include <Arduino.h>
#include <esp_random.h>
#include <stdint.h>

namespace {

struct PinState {
    uint32_t value;              // 0..999999; 0 = no PIN
    uint32_t expiresAtMillis;    // millis() when PIN expires
    uint8_t  attemptsRemaining;  // 1 while active; 0 when consumed
    uint8_t  failCount;          // consecutive mismatches in current window
    uint32_t lockoutUntilMillis; // 0 = not locked
};

PinState g_pin = {};

// Best-effort scrub of the PIN value: write through a volatile pointer with
// a read-back so the compiler cannot elide the store. The PIN is a 32-bit
// integer in BSS — there's no String / heap copy to worry about, but the
// volatile dance keeps this analogous to scrub_plaintext() in the HTTP
// handler module.
void scrub_pin_value() {
    volatile uint32_t* vptr = reinterpret_cast<volatile uint32_t*>(&g_pin.value);
    *vptr = 0u;
    (void)*vptr;
}

}  // namespace

std::atomic<bool> g_kosyncShowPinRequested{false};

bool kosync_pin_generate() {
    // Concept §4 step 5: respect existing lockout; never display a PIN
    // while locked. Caller is expected to translate `false` into 429 +
    // Retry-After. Calling is_locked_out() also self-clears an expired
    // lockout, so a stale lockoutUntilMillis won't permanently block us.
    if (kosync_pin_is_locked_out(nullptr)) {
        return false;
    }

    // 6-digit PIN: esp_random() mod 1,000,000 gives 0..999999 (the UI
    // zero-pads via %06u so leading-zero values render correctly).
    const uint32_t candidate = esp_random() % 1000000u;

    g_pin.value             = candidate;
    g_pin.expiresAtMillis   = millis() + kPinExpiryMs;
    g_pin.attemptsRemaining = 1;
    // NOTE: failCount is intentionally NOT reset here. A fresh-issue must
    // not erase prior mismatches — otherwise an attacker could loop
    // "request fresh + guess once" forever. The counter is only cleared
    // on Ok or on lockout entry (where it has just been used).

    g_kosyncShowPinRequested.store(true);
    return true;
}

bool kosync_pin_is_active() {
    // Read-only — must not mutate state (the HTTP handler uses this to
    // peek at status outside of a validate() call).
    if (kosync_pin_is_locked_out(nullptr)) return false;
    if (g_pin.value == 0u) return false;
    if (g_pin.attemptsRemaining == 0u) return false;
    // Wrap-safe expiry check: cast the unsigned delta to signed so that
    // subtraction across the millis() rollover boundary still produces a
    // meaningful sign.
    if (static_cast<int32_t>(g_pin.expiresAtMillis - millis()) <= 0) {
        return false;  // expired (state cleared lazily by validate/reset)
    }
    return true;
}

KosyncPinResult kosync_pin_validate(uint32_t candidate) {
    // Step 1: lockout check (per concept §4 step 1).
    if (kosync_pin_is_locked_out(nullptr)) {
        return KosyncPinResult::RateLimited;
    }

    // Step 2: no PIN issued / already consumed.
    if (g_pin.value == 0u || g_pin.attemptsRemaining == 0u) {
        return KosyncPinResult::NoActive;
    }

    // Step 3: expiry. Clear stale value lazily so the next request will
    // generate a fresh PIN through the normal step-5 path.
    if (static_cast<int32_t>(g_pin.expiresAtMillis - millis()) <= 0) {
        scrub_pin_value();
        g_pin.attemptsRemaining = 0;
        return KosyncPinResult::Expired;
    }

    // Step 4: match → consume + reset failCount.
    if (candidate == g_pin.value) {
        scrub_pin_value();
        g_pin.attemptsRemaining = 0;
        g_pin.failCount         = 0;
        return KosyncPinResult::Ok;
    }

    // Step 5: mismatch → increment, possibly enter lockout.
    g_pin.failCount += 1;
    if (g_pin.failCount >= kPinLockoutThreshold) {
        // Invalidate the active PIN so a re-issue is required after
        // lockout. Reset failCount for the next window.
        scrub_pin_value();
        g_pin.attemptsRemaining  = 0;
        g_pin.lockoutUntilMillis = millis() + kPinLockoutDurationMs;
        // Guard against the rare case where the wrap-safe delta would be
        // exactly zero — bump by 1 so is_locked_out() returns true at
        // least once.
        if (g_pin.lockoutUntilMillis == 0u) g_pin.lockoutUntilMillis = 1u;
        g_pin.failCount = 0;
        return KosyncPinResult::RateLimited;
    }
    return KosyncPinResult::Mismatch;
}

void kosync_pin_reset_state() {
    scrub_pin_value();
    g_pin.expiresAtMillis    = 0u;
    g_pin.attemptsRemaining  = 0u;
    g_pin.failCount          = 0u;
    g_pin.lockoutUntilMillis = 0u;
    g_kosyncShowPinRequested.store(false);
}

bool kosync_pin_is_locked_out(uint32_t* out_remaining_ms) {
    if (g_pin.lockoutUntilMillis == 0u) {
        if (out_remaining_ms) *out_remaining_ms = 0u;
        return false;
    }
    const int32_t delta = static_cast<int32_t>(g_pin.lockoutUntilMillis - millis());
    if (delta <= 0) {
        // Lockout window expired — clear it so future checks are cheap and
        // the next generate() call can succeed.
        g_pin.lockoutUntilMillis = 0u;
        if (out_remaining_ms) *out_remaining_ms = 0u;
        return false;
    }
    if (out_remaining_ms) *out_remaining_ms = static_cast<uint32_t>(delta);
    return true;
}

uint32_t kosync_pin_current_value() {
    return g_pin.value;
}

uint32_t kosync_pin_remaining_ms() {
    if (g_pin.value == 0u || g_pin.attemptsRemaining == 0u) return 0u;
    const int32_t delta = static_cast<int32_t>(g_pin.expiresAtMillis - millis());
    if (delta <= 0) return 0u;
    return static_cast<uint32_t>(delta);
}

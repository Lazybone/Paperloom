# KoSync — Maintainer Reference

Internal architecture, algorithm verification, threat model, and ops notes for the kosync feature. End-user documentation lives in [`README.md`](../README.md). Full design rationale lives in [`.codewright/plan/concept.md`](../.codewright/plan/concept.md); this doc is the self-contained maintainer view.

---

## 1. Architecture overview

| Module | Files | Responsibility |
|--------|-------|----------------|
| `KosyncClient` | `src/kosync_client.{h,cpp}` | HTTP transport: PULL `GET /syncs/progress/{hash}`, PUSH `PUT /syncs/progress`. Owns `WiFiClientSecure` per call, validates HTTPS-only, parses JSON with ArduinoJson, returns status code + populated struct or error string. Never throws. |
| `DocumentHasher` | `src/kosync_hash.{h,cpp}` | KoReader-compatible partial-MD5 of an EPUB. Helper functions, not a class — `kosync_compute_document_hash(path)` returns 32-char lowercase hex; `kosync_hash_is_valid(path, cachedSize)` cheap re-validation. |
| `KosyncSyncCoordinator` | `src/kosync_sync.{h,cpp}` | Orchestrator: PULL → bounds-check → divergence-check → optional conflict dialog → PUSH. Lazy-init singleton constructed once from `setup()` via `kosync_initialize_coordinator(BookReader&)`. Hot accessor `kosync_get_coordinator()`. Holds a non-owning `BookReader&`; constructs `KosyncClient` per-call from current `Settings` snapshot (no staleness window after a credential update). |
| `KosyncPinState` | `src/kosync_pin_state.{h,cpp}` | PIN-gate state machine: mint / validate / invalidate / lockout / deferred-show flag. File-scope `static PinState`; single-threaded main-loop invariant; cross-core-safe deferred-event-flag (`std::atomic<bool>`). |
| `kosync_http_handlers` | `src/kosync_http_handlers.{h,cpp}` | Web-UI endpoints (`GET/POST /api/kosync-settings`, `POST /api/kosync-register`). Split out of `wifi_upload.cpp` to keep that file in budget. Implements the C1 ordered flow (lockout-first → valid-PIN-consume → active+miss → fresh-issue). |
| `KosyncSettings` | additions in `src/settings.{h,cpp}` | Adds `kosyncServer`, `kosyncUser`, `kosyncKey` (MD5 hex), `kosyncDeviceName` to the `Settings` struct. Dual JSON-on-SD + NVS-mirror pattern, identical to existing fields. NVS keys: `kosyncSrv`, `kosyncUsr`, `kosyncKey`, `kosyncDev` (≤15 chars). |
| UI: setup screen | `src/ui/ui_reader_kosync_setup.{h,cpp}`, AppState `STATE_KOSYNC_SETUP` | On-screen-keyboard credential entry (server → user → password). Validates HTTPS, hashes password, zeroes plaintext buffer per §6.1, persists, returns to `STATE_READER`. |
| UI: conflict dialog | `src/ui/ui_reader_sync_conflict.{h,cpp}`, AppState `STATE_SYNC_CONFLICT` | Side-by-side local vs remote display. Three buttons → `resolveConflict(true)` / `resolveConflict(false)` / `clearBusy()` + cancel-toast. |
| UI: PIN prompt | `src/ui/ui_kosync_pin_prompt.{h,cpp}`, AppState `STATE_KOSYNC_PIN_PROMPT` | Modal screen showing the freshly minted 6-digit PIN. Auto-exits on consume / 60 s expiry / lockout entry. |
| Toast | `src/ui/ui_toast.{h,cpp}` | Generic short banner: `ui_toast_show(msg, durationMs=2000, isError=false)`. Used by all sync surfaces. |
| TLS trust | `include/tls_certs.h`, `src/tls_certs.cpp` | `extern const char PAPERLOOM_TRUSTED_ROOTS[]`. Shared by OTA and kosync. |

Lifetime / coupling rules:
- `KosyncSyncCoordinator` holds a non-owning reference to `BookReader`. `BookReader` MUST NOT hold a reference back to the coordinator — this is enforced by a top-of-header comment in `kosync_sync.h`. If sync-aware behavior is ever needed in the reader, add a separate `SyncStateListener` interface.
- The coordinator does NOT cache a `KosyncClient` field. Every `syncNow()` / `resolveConflict(...)` re-reads `Settings.kosyncServer / kosyncUser / kosyncKey` and constructs a fresh client. Credential updates take effect immediately — no reload step, no power-cycle.

---

## 2. KoReader partial-MD5 algorithm — fidelity & verification

### Upstream reference

- **Function**: `util.partialMD5(file)`
- **Path**: `frontend/util.lua` in [`koreader/koreader`](https://github.com/koreader/koreader)
- **TODO (BLOCKER for WP-14 acceptance)**: pin the exact upstream commit SHA against which Paperloom's port was validated. Use the latest KoReader stable release tag at the time of implementation. Record:
  - Upstream tag: `<TODO — e.g. v2025.04>`
  - Upstream commit SHA: `<TODO — full 40-char hex>`
  - Date of verification: `<TODO — YYYY-MM-DD>`

### Algorithm

Read up to 12 chunks of 1024 bytes from offsets `step << (i+1)` for `i = -1 .. 10` with `step = 1024` (i.e. offsets 1024, 2048, 4096, …, up to `1024 << 11`). Feed every chunk into a single MD5 context. Return the 32-char lowercase hex digest.

A short read or EOF terminates the loop — the digest is whatever has been fed in so far. Files smaller than 2048 bytes naturally produce a digest from the available chunks.

### Paperloom port — buffering decision

Paperloom buffers all chunks (≤ 12 × 1024 = 12 KiB) into a single `std::vector<uint8_t>` and calls `mbedtls_md5()` one-shot, instead of the streaming `mbedtls_md5_starts / _update / _finish` triple.

**Why one-shot:** the toolchain `espressif32 @ 6.4.0` ships `libmbedcrypto.a` with **only** `mbedtls_md5()` linkable. The streaming wrappers (`_starts_ret`, `_update_ret`, `_finish_ret`) are declared in the public headers but not present in the linked archive — referencing them produces a link-time undefined symbol. Buffering is functionally equivalent: the digest depends only on the concatenated byte stream and the final length, not on the size of the `update` calls.

**Trade-off:** an extra 12 KiB heap allocation per hash. Acceptable: the hash is computed at most once per book (cached in the progress JSON), and PSRAM is available.

### Verification procedure

1. Pick an EPUB that is identical on both KoReader and Paperloom (same file, same byte count).
2. **KoReader side**: open the book; from the KoReader debug console / log, capture the value KoReader writes when it computes `partialMD5` for the book. If your build doesn't surface it directly, add a one-line `logger.dbg()` and recompile, or read it out of the `<KoReader-config>/history.lua` cache entry.
3. **Paperloom side**: open the same book; the firmware logs the value returned by `kosync_compute_document_hash()` at debug level on the serial console (search for `[kosync] hash=`).
4. Compare byte-for-byte. They MUST match.

### Recorded verification

```
Test EPUB:               <TODO — filename + size bytes>
KoReader version / SHA:  <TODO — v2025.04 / <commit>>
KoReader hash:           <TODO — 32-char lowercase hex>
Paperloom firmware ver:  <TODO — semver from CHANGELOG>
Paperloom hash:          <TODO — 32-char lowercase hex>
Match:                   <TODO — yes / no>
Verified by / date:      <TODO — handle / YYYY-MM-DD>
```

If `Match: no`, do not merge the feature. Investigate offset arithmetic, byte ordering, and chunk boundaries against the upstream Lua source before adjusting the C++ port.

---

## 3. TLS trust bundle

- Declaration: `extern const char PAPERLOOM_TRUSTED_ROOTS[];` in `include/tls_certs.h`.
- Definition: PEM-concatenated string literal in `src/tls_certs.cpp`.
- Current contents:
  - **USERTrust ECC Certification Authority** (Sectigo) — required for some OTA paths.
  - **ISRG Root X1** — Let's Encrypt. Covers `kosync.eu` (the public free server) and most self-hosted deployments behind Let's Encrypt.
  - **DigiCert Global Root CA** — broad coverage for self-hosted servers behind DigiCert intermediates.
- Used by both OTA (`ota_update.cpp`) and kosync (`KosyncClient::pullProgress/pushProgress`) — single source of truth.
- **CA rotation procedure**: when a root expires or rotates, append the new PEM to `src/tls_certs.cpp` (keep the old one until devices in the field have updated past the cutover date), bump the firmware version, and ship a release. The OTA path itself depends on this bundle, so do not retire a root before all reachable devices have rolled past the version that introduced its replacement.

`NEVER setInsecure()` anywhere in the codebase — enforced by review checklist.

---

## 4. PIN gate parameters

Tunable in `src/kosync_pin_state.h` (compile-time constants, all `static constexpr`):

| Constant | Value | Meaning |
|----------|-------|---------|
| `kPinExpiryMs` | `60'000` | PIN auto-expires 60 s after generation. |
| `kPinLockoutThreshold` | `3` | Consecutive misses that trigger lockout. |
| `kPinLockoutDurationMs` | `5 * 60'000` | Lockout window: 5 minutes. |

Rationale notes (also inline in the header):
- 60 s expiry balances "user has time to read & type" vs "attacker has narrow brute-force window".
- 3-strike threshold is conservative — accidental fat-finger doesn't lock you out.
- 5 min lockout is long enough to make a six-digit-keyspace brute-force impractical (~1e6 keys × 5 min batches → centuries), short enough that an honest user just waits.

The lockout is **global** (single-PIN scope on a single-user device); not per-IP.

---

## 5. Divergence thresholds (conflict detection)

Tunable in `src/kosync_sync.cpp` (file-scope `static constexpr`):

| Constant | Value | Meaning |
|----------|-------|---------|
| `kPageDeltaThreshold` | `2` | Conflict iff `|local.page − remote.page| > 2` within the same chapter. Tolerates 1–2 pages of normal drift. |
| `kPercentageDeltaThreshold` | `0.01f` | Conflict iff `|local.percentage − remote.percentage| > 1.0 %`. Accounts for rounding noise. |
| `kTimestampToleranceSec` | `300` | Accept remote timestamps up to 5 min in the future (clock skew). |
| `kTimestampMaxAgeSec` | `5 * 365 * 24 * 3600` | Reject remote timestamps older than 5 years (treated as malformed). |

Chapter inequality is always a conflict — the page / percentage thresholds only soften within-chapter drift.

Revisit after real-world testing. Constants are compile-time only; not exposed to user settings to keep the surface area small.

---

## 6. Threat model & trust boundaries

### 6.1 Plaintext password lifecycle

- Plaintext exists only between "user typed it" and "MD5 computed". Then the buffer is zeroed via the optimization-proof pattern (volatile pointer + read-back), and the `String` is reassigned to `String()`.
- Three call sites enforce this: `STATE_KOSYNC_SETUP` save (WP-5), `POST /api/kosync-settings` handler (WP-6a), `POST /api/kosync-register` handler (WP-6b).
- `Settings.kosyncKey` stores only the 32-char lowercase hex MD5; the plaintext never reaches NVS or SD.
- `GET /api/kosync-settings` NEVER returns `kosyncKey`. Returns `{server, user, deviceName}` only.

### 6.2 PIN gate boundaries

- **On-device setup is NOT PIN-gated.** Physical access to the device is the trust boundary for credential writes via `STATE_KOSYNC_SETUP`. A PIN on the same screen that displays it would be self-defeating.
- **Web-UI credential writes ARE PIN-gated.** `POST /api/kosync-settings` and `POST /api/kosync-register` require a fresh 6-digit PIN displayed on the e-paper. This protects against LAN peers writing credentials and incidentally closes CSRF on the same endpoints (a cross-LAN page cannot guess the freshly displayed code).
- **`GET /api/kosync-settings` is NOT PIN-gated.** Returns username + server URL + device name to anyone on the LAN. Documented trust boundary: the LAN is trusted.
- **PIN observer (same-room) threat.** The 6-digit PIN displayed on the e-paper is visible to anyone glancing at the device during the 60 s window. The model assumes anyone in the room is trusted. Single-use semantics prevent reuse after one valid POST; the lockout prevents brute force.

### 6.3 Logging discipline

- NEVER log: password plaintext, `kosyncKey`, full request/response bodies, `X-Auth-Key` header value, the PIN value, the full POST body of credential endpoints.
- DO log: validation outcome class (`pin_ok` / `pin_expired` / `pin_mismatch` / `pin_rate_limited` / `pin_required`), HTTP status codes, redacted body excerpt (≤200 chars with `kosyncKey` and password fields masked), TLS handshake outcome (cert subject / issuer on failure).

### 6.4 Kosync field write isolation

`Settings.kosyncServer`, `kosyncUser`, `kosyncKey`, `kosyncDeviceName` are writable ONLY via:
- `POST /api/kosync-settings` (PIN-gated)
- `STATE_KOSYNC_SETUP` (physical-access-gated)

Any future generic settings-import / restore / JSON-upload feature MUST either exclude these four fields from the import path, OR re-trigger the PIN gate before applying them. The WP-6a implementation grep-verifies that no other handler writes these fields.

### 6.5 Document hash trust

`document_hash` flows from `kosync_compute_document_hash()` (trusted local source) and is re-validated against `^[a-f0-9]{32}$` before URL construction. The server's response `document` field is NEVER reused for URL construction — defense against a malicious / buggy server returning a path-traversal token.

### 6.6 MD5 caveat

MD5 is the kosync-protocol-mandated hash for the auth key. We cannot upgrade without server cooperation. Mitigation: user-facing docs (`README.md` → KoReader Sync → Things to know) advise picking a unique kosync password not reused elsewhere, and rotating it every few months. The MD5 caveat is a documented protocol limitation, not a Paperloom design choice.

---

## 7. Build-size delta (M5 gate)

Before merging this feature to `main`:

1. Check out the `pre-kosync-feature` tag (or the last commit on `main` before the kosync work landed).
2. `pio run -t size -e default` → record `Flash:` and `RAM:` baseline.
3. Check out the feature branch tip.
4. `pio run -t size -e default` → record delta.
5. Append the result to this section:

```
Baseline ref:    <tag or SHA>
Baseline Flash:  <bytes>
Baseline RAM:    <bytes>
Feature Flash:   <bytes>          delta: <+bytes / %>
Feature RAM:     <bytes>          delta: <+bytes / %>
Date / build:    <YYYY-MM-DD / commit>
```

**Flag for review** if:
- Flash growth > 50 KB
- Static SRAM growth > 5 KB

These are soft limits; growth beyond them is allowed but must be justified in the PR description (e.g. unavoidable mbedtls symbol pull-in).

---

## 8. Backward compatibility

### Progress JSON cache version

- `cache_version` bumped from `1` to `2`.
- v2 adds two fields: `kosync_document_hash` (32-char lowercase hex) and `kosync_last_sync` (unix timestamp).
- **Forward compat (old firmware reading v2 JSON)**: existing loaders ignore unknown fields; the two extra entries are harmless.
- **Backward compat (new firmware reading v1 JSON)**: missing `kosync_document_hash` triggers a re-compute on first sync; missing `kosync_last_sync` defaults to 0 (treated as "never synced"). No data loss.
- On save, the new firmware always writes `cache_version: 2`. A downgrade-then-upgrade cycle is lossless.

### ButtonAction enum

Append-only: `BTN_ACTION_KOSYNC_SYNC = N+1` at the tail, where `N` is the previous last value. Update the `BTN_ACTION_COUNT` sentinel. **Never insert mid-enum** — that would silently remap existing user bindings persisted in NVS / settings JSON.

### Settings JSON

New kosync fields default-fill on missing keys (rules in `concept.md` §4 — KosyncSettings Extension): `kosyncServer` defaults to `https://kosync.eu` only when never touched; `kosyncKey` defaults to empty + sets `kosyncCredentialsInvalid` if malformed; `kosyncDeviceName` computed once from MAC if absent.

---

## 9. Manual test plan (post-implementation acceptance)

### Golden path

1. Flash a fresh firmware. Configure WiFi. Configure kosync credentials via web-UI (verify PIN gate works — wrong PIN, expired PIN, lockout-after-3, recovery after 5 min).
2. Bind the BOOT button to "Sync Fortschritt".
3. Open Book A. Tap the BOOT button → expect `Sync ok (neu)` (first sync — server has no progress).
4. Read forward 20 pages. Tap BOOT → expect `Sync ok`.
5. On a second device (or via `curl` to kosync.eu directly), modify Book A's progress to a position 100 pages ahead.
6. Tap BOOT on the Paperloom → expect the conflict dialog with local-vs-remote comparison.
7. Pick "Remote übernehmen" → expect `Sync ok (Lokal aktualisiert)`, reader jumps forward to the remote position.
8. Power-cycle the device. Re-open Book A → expect to be at the synced position (persistence survives).

### Negative paths

| Scenario | Expected toast |
|----------|----------------|
| WiFi off, trigger sync | `Sync fehlgeschlagen: Kein WLAN` |
| Server URL points to unresolvable host | `Sync fehlgeschlagen: Server nicht erreichbar` |
| Server URL points to wrong port / refused connection | `Sync fehlgeschlagen: Server nicht erreichbar` |
| Wrong password | `Sync fehlgeschlagen: Login ungültig` |
| Server returns 500 | `Sync fehlgeschlagen: Serverfehler` |
| Server returns malformed JSON | `Sync fehlgeschlagen: Serverfehler` |
| Remote progress has `chapter` beyond book end | `Sync fehlgeschlagen: Serverfehler` (bounds check) |
| EPUB replaced on SD card between syncs | New hash computed; next sync starts fresh (404 → `Sync ok (neu)`) |
| Conflict dialog → Abbrechen | `Sync abgebrochen` |
| Trigger sync from inside settings menu | No-op (precondition guard — concept §7) |
| Trigger sync with no book open | `Kein Buch geöffnet` |
| 3 wrong PINs in a row via web-UI | `429 rate_limited` with `Retry-After: <s>` header |
| Web-UI POST with extra `server` field on `/api/kosync-register` | `400 unknown_field` |

### PIN flow

1. Submit credentials with no `pin` → expect PIN to appear on e-paper, browser shows "PIN required".
2. Submit credentials with the displayed PIN within 60 s → expect `200 ok`, credentials persisted.
3. Submit credentials with the PIN after 65 s → expect re-issued PIN on e-paper, `401 pin_required`.
4. Submit credentials with a wrong PIN 3 times → expect `429 rate_limited` after the 3rd, lockout for 5 min, no new PIN displayed during lockout.

---

## 10. Known limitations / future work

- **Cross-app `progress` field is approximate.** Paperloom emits its own encoding; only `percentage` aligns reliably with stock KoReader. Future work: align with KoReader's XPath-based progress format for exact position-jumping across apps.
- **No automatic sync.** User requested manual-only. No on-open / on-close / on-page-turn / periodic-background trigger. If automatic sync is ever added, it must remain opt-in.
- **No connection reuse across the conflict dialog.** PULL and PUSH share one TLS connection within `syncNow()` (convergent path). For the conflict path, the dialog gap is user-driven minutes — far longer than typical server TCP idle (~30 s) — so a fresh handshake is taken for the post-conflict PUSH. Documented in `concept.md` §9; canonical mitigation for the would-be info-leak / fingerprint window of holding TLS open across a dialog.
- **No sync resume.** Interrupted transfers are treated as errors; user re-triggers.
- **No backup / history.** Only the latest progress is held server-side. Conflict resolution overwrites — no merge.
- **Password rotation is manual.** No reminder, no expiry. Operators with stricter rotation policies should script a periodic re-run of `STATE_KOSYNC_SETUP` or the web-UI flow.

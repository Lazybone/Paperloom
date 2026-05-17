# Changelog

All notable changes to this firmware will be documented in this file.

## Unreleased

## v0.2.5 — 2026-05-17

### Added
- **`ChangeKind::TapPulse`** intent in the partial-update API, mapped to
  `MODE_DU4` (4-grey, ~50–80 ms). Reserved for future tap-feedback
  consumers; not wired into the Library today (no existing tap
  animation to migrate).
- **Per-zone anti-ghost counter** (`_framesSincePerZone[]`). Each Reader
  zone now tracks its own frames-since-last-full and escalates to a
  GC16 clean of its own rect only — header glyph-ticks no longer drag
  the body's ghost budget into a panel-wide refresh. Threshold for the
  header zone alone is the new `REFRESH_INTERVAL_HEADER` (default 30);
  body/footer stay on `REFRESH_INTERVAL_READER`. A panel-wide full
  clear runs only when `Zone::FullScreen` itself escalates or when 2+
  dirty zones escalate simultaneously.
- **Persistent landscape rotation buffer** (`_rotation_buf`, 256 KB
  PSRAM, allocated once in `display_init`). Removes the per-flush
  `malloc`/`free` that fragmented the heap on rapid page-turns and
  saves ~5–10 ms per multi-zone flush. Falls back to per-flush malloc
  with a `WARN` log if PSRAM is exhausted at boot.
- **`flushSettingsRow` helper** in `ui_settings.cpp` for per-row
  partial flushes via `Zone::Overlay`. Pilot-wired to the Page Numbers
  toggle; broader per-row migration is deferred (would require
  factoring row-drawing out of `ui_settings_draw`).
- **`tall_overlay_begin/_flush` helpers** in `ui_reader.cpp` for body-
  only overlays (header-bottom y=66 to footer-top y=910, height 844).
  Used by the menu overlay; documents the rotate-only-the-rect bounds
  contract so a second consumer (currently none) can adopt it safely.

### Changed
- **Menu overlay (`ui_reader_menu_draw`)** now uses `Zone::Overlay`
  with the body-only rect instead of `Zone::FullScreen`. Reader
  header and footer chrome stay visible during the menu — saves
  ~80–150 ms per menu open/close. TOC/Bookmarks/GoTo/Picker overlays
  intentionally stay on `Zone::FullScreen` because they paint their
  own `drawHeader`/`drawBottomBar` chrome at y<66 and y>910 that the
  body-only rect would hide.
- **Overlay-rect-empty diagnostic** in `display_flush` is now
  unconditional (previously gated behind `DISPLAY_FLUSH_LOG`). Catches
  a missed `display_set_overlay_rect()` immediately on serial.
- **Flush-log line** prints per-zone counters
  (`hdr=%d body=%d foot=%d`) in place of the single
  `frames_since_full=%d`.

### Removed
- **Deprecated `display_update_*` shims** — `display_update`,
  `display_update_medium`, `display_update_fast`,
  `display_update_partial`, `display_update_mode`,
  `display_update_reader_body`. All callers had already migrated to
  the intent API. `display_update_sleep` (terminal sleep-image latch)
  stays. `needsRedraw` flag in `main.cpp` also stays — kept as an
  atomic signal for a future Touch-ISR migration.

### Notes
- The plan and supporting analysis live in
  `docs/superpowers/plans/2026-05-17-fast-partial-refresh.md`
  alongside the t5s3-analysis reference docs and the round-1/round-2
  reviews under `docs/superpowers/specs/`.

## v0.2.4 — 2026-05-13

### Added
- **Sleep entry in the reader menu**: open the reader menu (tap centre)
  and the new bottom entry "Sleep" puts the device into deep sleep —
  the same code path as the side button's hold-to-sleep gesture.
  Resume on wake reopens the same book at the same page.

### Fixed
- Wake-from-sleep no longer falls back to the library when the device
  was last on a KoSync screen. The wake-time `kMaxKnownState` guard was
  pinned to `STATE_WIFI_KEYBOARD`, so any AppState added after that
  point (`STATE_KOSYNC_SETUP`, `STATE_SYNC_CONFLICT`,
  `STATE_KOSYNC_PIN_PROMPT`) was silently rejected as "invalid
  savedState" on wake. Now points to the actual last persistable state,
  and a file-scope `static_assert` catches future drift at build time.
- Reader menu no longer renders the "Tap outside to resume" hint and
  "This page is bookmarked" notice when the menu fills the screen
  (e.g. when navigation history adds a Back row, making 9 entries
  total). They previously overlapped the bottom menu items visually
  and the hint touch zone collided with the last row, so tapping the
  hint to resume could trigger the row beneath instead.

## v0.2.3 — 2026-05-13

### Fixed
- Web flasher: fix the post-install **Logs** panel collapsing every line
  onto a single paragraph with the wrong colour scheme. esp-web-tools
  attaches an open shadow root on each of its custom elements (the
  install dialog, `ewt-console`, every `ew-*` button) and injects its
  scoped CSS as an inline `<style>` via `shadowRoot.innerHTML`. The
  flasher CSP only allowed `style-src 'self'`, so those `<style>`
  blocks were blocked: the dark `:host` terminal styling and the
  `.log { white-space: pre-wrap }` rule that turns `\n` into line breaks
  never applied. Relaxed `style-src` to `'self' 'unsafe-inline'` (matches
  the optimizer page). Trade-off is bounded — the inline styles only
  exist inside the third-party shadow trees and don't widen the page's
  own surface.
- Silence `DBGTRACE mark stage=…` Serial spam in normal builds. The
  marker was instrumentation for chasing a reset bug; the NVS write
  was already gated behind `-DDEBUG_TRACE_NVS`, but the Serial
  `printf` next to it fired unconditionally and showed up on the web
  flasher's serial console after install. Both are now gated together.
  Boot crash trace (`debug_trace_boot_report`) still prints, so
  recovering the last stage after a panic continues to work.
- Bump the visible page version chip on the GitHub Pages hub
  (`site/index.html`) and the flasher (`site/flasher/index.html`)
  to match the firmware version. `tools/build.py` now sweeps both
  mastheads alongside `manifest.json` and `README.md`, so the deployed
  site no longer drifts behind on the next release.

## v0.2.2 — 2026-05-13

### Added
- **GitHub Pages Workshop**: the install page at `lazybone.github.io/Paperloom/`
  is now a hub with two cards. The existing web installer moved to
  `/flasher/`; a new **EPUB optimizer** at `/optimizer/` runs a full
  client-side optimisation pipeline ported from
  [b1rdmania/epubkit](https://github.com/b1rdmania/epubkit). The optimizer
  quantises images to the 16-gray Paperloom palette, strips embedded fonts,
  prunes unused CSS, repairs the TOC, normalises text (whitespace,
  OCR-ligatures, smart quotes, NFC), and re-packages the EPUB. Everything
  runs in the browser tab — no upload, no analytics, no server.
- Shared design layer at `site/shared/` (`design.css`, `fonts.css`).
  Hub, flasher, and optimizer all import the same OKLCH "Paper/Lamplight"
  tokens, Fraunces + Inter typography, and section primitives.
- `web/optimizer/` npm workspace (TypeScript + esbuild + Vitest +
  GoogleFonts SRI-friendly @import). Output bundles land in
  `site/optimizer/optimizer.js` + `site/optimizer/image-worker.js`,
  enforced under a 200 KB gzipped budget by `scripts/size-check.mjs`.
- GitHub Actions workflow `.github/workflows/optimizer-build.yml` that
  installs, lints, tests, builds, gates bundle sizes, and commits the
  static artifacts back to `site/optimizer/` for GitHub Pages.
- `LICENSES/MIT-epubkit.txt` records the upstream MIT attribution.

### Changed
- `site/index.html` is now the Workshop hub; the install card moved to
  `/site/flasher/`. esp-web-tools v10.2.1 is pinned with SRI on the
  entry module. CSP on every page is explicit per surface (hub strict,
  flasher allows unpkg + GitHub release hosts, optimizer denies all
  network egress).
- Page-local CSS for each surface lives in a sibling `.css` file (no
  more inline `<style>` blocks). The strict `style-src 'self'` CSP no
  longer breaks the rendered pages.
- README installer section now mentions the hub layout and points users
  to the optional EPUB optimizer.

### Changed
- Renamed the on-device "WiFi Upload" screen to **"WiFi Manager"** — it
  has always done more than upload (file browser, full settings panel
  including WiFi, reading, frontlight, buttons), and the old label
  undersold it. Internal C++ symbols (`wifi_upload_*`) kept for the
  diff's sake; only the user-facing strings + comments changed.

### Removed
- The legacy duplicate `manifest.json` at the site root. Firmware OTA
  reads `https://api.github.com/repos/Lazybone/Paperloom/releases/latest`
  directly (see `src/ota_update.cpp:165`); only the esp-web-install
  button consumed `manifest.json` and it now reads
  `site/flasher/manifest.json` via its relative attribute.
- The `docs/` directory was renamed to `site/`. GitHub Pages is now
  served via a GitHub Actions workflow that publishes `site/` instead of
  the legacy "deploy from /docs" branch source. The folder name now
  honestly describes what's inside (the deployed web app, not docs).

## v0.2.1 — 2026-05-12

### Fixed
- OTA update check no longer fails silently after GitHub rotated its CA
  chain. `api.github.com` now serves through Sectigo / USERTrust ECC and
  the release CDN through Let's Encrypt / ISRG Root X1; the firmware
  previously pinned only DigiCert Global Root CA, so both TLS handshakes
  failed and the UI rendered the failure as "Up to date (v0.2.0)".
  Trust bundle now ships USERTrust ECC + ISRG Root X1 + DigiCert as a
  fallback, so OTA detects new releases again.

### Security
- `SECURITY.md`: OTA hardening note rewritten to describe the multi-root
  trust bundle and the post-redirect host whitelist.

## v0.2.1 — 2026-05-12

### Added
- BOOT button (GPIO 0) gestures are now configurable from Settings -> Buttons.
  Tap / double-tap / hold each map to a `ButtonAction` (None, Light toggle,
  Library, Sleep, Next page, Prev page, Menu). Long-press still falls back to
  Sleep when the button is disabled so the device can always be powered down.
- Settings now mirror to NVS (namespace `ereader_set`). On boot the firmware
  recovers from NVS when `/.settings.json` is missing, parse-fails, or carries
  a too-new schema, then rewrites the SD copy.
- On-device picker overlay in Settings for button actions, brightness, and
  font family. Picker changes save immediately instead of on screen-exit.

### Changed
- Web settings page redesigned: segmented controls (XS..XL font size,
  Tight..Loose line spacing), toggle switches, range slider with live percent
  for brightness, section icons, password reveal, body-dimming for disabled
  sections, sticky save bar with dirty indicator and Discard / Save buttons.
- Settings file moved from `/books/.settings.json` to `/.settings.json`; the
  legacy path is migrated and removed on first boot of v0.2.1.
- Settings UI label "User Button" -> "IO48 Button" to match the actual
  PCA9535 IO12 hardware.
- Release flow: `python3 tools/build.py X.Y.Z` then manual `gh release create`
  replaces the previous tag-driven CI publish.
- `CONTRIBUTING.md` rewritten; bug-report and pull-request templates refreshed.

### Fixed
- Hardened the WiFi-connect "dots" animation buffer: replaced the unbounded
  `strcat` loop with `snprintf` and added explicit length guards. The previous
  code was safe by happenstance (mod-4 counter) but a future refactor could
  have overflowed.
- Removed a redundant `dy < 0` bounds check in `raw4_accumulate` that was
  always false at the call site.

### Security
- `SECURITY.md` rewritten with concrete hardening details: path-validator
  rules (no `..`, no `\`, no CR/LF, no NUL, no dot-prefixed segments isolating
  `/.settings.json`), atomic writes, 200 MB upload cap, settings API never
  returning `wifiPass`, snprintf hardening pass, and an explicit roadmap note
  for plaintext SD-card credentials (physical-access risk).

## v0.2.0 — 2026-05-12

### Changed
- Reader fonts replaced.  FiraSans (sans) + NotoSerif (serif) removed; new
  families Lexend Deca (Sans), Literata (Serif), and Bitter (Slab) — all SIL
  OFL — pre-rendered via `tools/fontconvert.py` from each upstream Regular
  face; resulting headers are checked into `src/font_*.h`.  Latin-1
  Supplement coverage added (German umlauts, accents).
- UI chrome (header, footer, library, settings, menus) now renders in
  Inter — also SIL OFL — independent of the reader-family selection.
  `display_set_font_size()` now uses Inter; `display_set_font(level, family)`
  is the reader path.
- Settings schema bumped v2 → v3.  The boolean `serifFont` field is
  replaced by `uint8_t fontFamily` (0=Sans, 1=Serif, 2=Slab).  `settings.cpp`
  migrates an existing v2 file on first load (true → Serif, false → Sans);
  the Web UI accepts both keys for backwards-compat clients.

## v0.1.0 — Initial Release

First release of the T5S3 PRO-only firmware fork.

### Hardware
- LilyGo T5S3-4.7-e-paper-PRO (ESP32-S3-WROOM-1-N16R8, 16 MB flash, 8 MB OPI PSRAM)
- ED047TC1 4.7" 960×540 e-paper panel via TPS65185 PMIC (vroland/epdiy@2.0.0, `epd_board_v7`)
- GT911 capacitive touch on shared I²C-0 (SDA=39, SCL=40)
- BQ27220 fuel-gauge (I²C, 0x55)
- microSD over SPI (MOSI=13, MISO=21, SCLK=14, CS=12)
- PCF85063 RTC, BQ25896 charger
- LoRa SX1262 + GPS held in safe-disabled state at boot
- Frontlight on GPIO 11 (LEDC ch4)
- BOOT button (GPIO 0) + side button via PCA9535 IO12

### Features

**Reading**
- EPUB parsing with chapter navigation and pagination
- 5 font size levels (XS, S, M, M-L, L)
- 3 reader font families: Sans (Lexend Deca), Serif (Literata), Slab (Bitter) — all SIL OFL, see `LICENSES/`
- Latin-1 Supplement glyph coverage (German umlauts, accents)
- 5 line-spacing presets (Compact → Extra)
- Partial refresh with configurable cleanup cadence
- Bookmarks, progress bar, chapter / page indicators

**Library**
- List + poster view, filter tabs (ALL / NEW / READING / DONE)
- Continue Reading banner

**Power & sleep**
- Deep sleep on inactivity (configurable timeout)
- BOOT button ext1 wake source
- Sleep image rotation from `/sleep` SD folder with built-in fallback
- Wake feedback banner before UI redraw

**Network**
- WiFi book + sleep-image upload via on-device web UI
- OTA firmware update via GitHub Releases (`firmware.bin` asset)

**Inline EPUB images**
- JPEG/JPG inline images via SD-backed raw4 pre-render cache
- PNG inline images skipped in this release (PNG raw4 path corrupts heap — fix pending)

### Build system
- Single PlatformIO environment: `default`
- `gh_release` env extends `default` and injects `FIRMWARE_VERSION` from `RELEASE_TAG`
- `tools/patch_epdiy.py` PlatformIO pre-script patches `vroland/epdiy@2.0.0` so its `epd_board_v7::epd_board_init` tolerates an already-installed I²C driver and skips `i2c_param_config`
- Release builds via `tools/build.py`; manual `gh release create` for publishing

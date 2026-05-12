# Changelog

All notable changes to this firmware will be documented in this file.

## Unreleased

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

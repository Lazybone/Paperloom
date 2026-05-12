# Paperloom

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange)](https://platformio.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![ESP32-S3](https://img.shields.io/badge/MCU-ESP32--S3-green)](https://www.espressif.com/en/products/socs/esp32-s3)

EPUB reader firmware optimised for two LilyGo boards sharing the same ESP32-S3 + ED047TC1 4.7" 960×540 panel:

- [**LilyGo T5 E-Paper S3 Pro**](https://lilygo.cc/products/t5-e-paper-s3-pro) — capacitive touch, frontlight, side button, BQ27220 fuel-gauge, microSD, RTC, charger, LoRa/GPS hardware (held safely disabled at boot).
- [**LilyGo T5 E-Paper S3 Pro Lite**](https://lilygo.cc/products/t5-e-paper-s3-pro-lite) — same SoC and panel, lighter peripheral set; non-Lite features (e.g. fuel-gauge, frontlight) gracefully degrade when the underlying hardware is absent.

Portrait UI, SD-backed library, on-device WiFi setup with on-screen keyboard, browser-based file manager (**Paperloom**), OTA updates pinned to GitHub releases, sleep-image gallery.

Version: **v0.2.1** · PlatformIO env: `default`

---

## Highlights

- Reader fonts: **Lexend Deca**, **Literata**, **Bitter**, **ChareInk7SP**, plus **Inter** as a reader-selectable face — all SIL OFL, picked for e-ink legibility. **Inter** also drives all UI chrome.
- 5 font sizes (XS / S / M / M-L / L) × 5 line-spacing presets, swappable mid-book.
- EPUB parser with chapter navigation, on-the-fly pagination, bookmarks, progress bar, swipe + tap controls.
- **Paperloom** browser file manager — upload / download / mkdir / delete across SD, plus full Settings panel synced to device.
- On-device WiFi setup: scanner + QWERTY on-screen keyboard. No need to pre-flash credentials.
- OTA firmware updates pinned to GitHub releases (HTTPS, DigiCert root, partition-size guard, 64 KB response cap).
- Sleep-image rotation from `/sleep` with built-in fallback. Configurable inactivity timeout (1–240 min).
- Library: list + poster view, ALL / NEW / READING / DONE filters, Continue Reading banner, cover-art posters, per-book progress badges.
- Side-button gestures: tap / double-tap / hold, each user-mapped (Light / Library / Sleep / Next / Prev / Menu).
- Hardened web layer: path-traversal guard, atomic `.tmp` + rename uploads (200 MB cap), CRLF-injection-safe `Content-Disposition`.
- Versioned settings schema (`schemaVersion=2`) — refuses to load unknown future schemas.
- Inline EPUB JPEGs render from pre-baked **raw4 SD cache** — no live decode on page turn.
- Performance tuned for ESP32-S3: PSRAM DEFLATE buffers, packed-nibble `memset` fill (~100× faster), 16 KB OTA chunks matched to TLS records, 20 kHz frontlight PWM.
- Crash guard with "previous session reset" banner; orphan `.tmp` GC on every boot.

---

## Quick Start

```bash
# 1. Hardware setup
#    - Insert FAT32-formatted microSD
#    - Optional: pre-load EPUBs into /books, sleep images into /sleep

# 2. Flash firmware
cp include/config.h.example include/config.h    # WIFI_SSID/PASS optional
pio run -t upload                                # build + flash
pio device monitor -b 115200                     # serial logs

# 3. First boot
#    - Settings → WiFi Setup → scan + select your network → enter password on
#      the on-device QWERTY keyboard. No need to pre-flash credentials.
#    - Settings → WiFi Upload → device serves Paperloom on port 80; open
#      http://<device-ip>/ in any browser to upload books.
```

One-shot wrapper: `python tools/flash.py`. Release build: `pio run -e gh_release` (consumes `RELEASE_TAG` env var). Cutting a versioned release: `python3 tools/build.py 0.2.0` — bumps `FIRMWARE_VERSION` in `include/config.h` and the `Version:` line in this README, builds `build/Paperloom-0.2.0.bin` (download-friendly name) plus `build/firmware.bin` (exact asset name the on-device OTA checker requires), rolls the `## Unreleased` block of `CHANGELOG.md` into a `## v0.2.0` section, and writes `build/release_notes_0.2.0.md` for the GitHub release body. Append `--publish` to invoke `gh release create` automatically; `--draft` to publish as draft.

Prereqs: PlatformIO CLI, USB-C cable, FAT32 microSD (any size).

---

## Hardware

Targets two LilyGo variants — [T5 E-Paper S3 Pro](https://lilygo.cc/products/t5-e-paper-s3-pro) and [T5 E-Paper S3 Pro Lite](https://lilygo.cc/products/t5-e-paper-s3-pro-lite). They share the SoC, panel, touch, storage, and WiFi stack; the Lite drops a subset of the peripheral chips (fuel-gauge, frontlight, LoRa/GPS daughterboard) and the firmware skips those code paths when the chip isn't reachable.

| Block          | Detail                                                                        | Pro | Pro Lite |
| -------------- | ----------------------------------------------------------------------------- | --- | -------- |
| MCU            | ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB OPI PSRAM)                          | ✔   | ✔        |
| Display        | ED047TC1 4.7" 960×540 via TPS65185 PMIC (vroland/epdiy@2.0.0, `epd_board_v7`) | ✔   | ✔        |
| Touch          | GT911 on I²C-0 (SDA=39, SCL=40, addr 0x5D)                                    | ✔   | ✔        |
| Storage        | microSD SPI — MOSI=13, MISO=21, SCLK=14, CS=12                                | ✔   | ✔        |
| WiFi           | 2.4 GHz 802.11 b/g/n                                                          | ✔   | ✔        |
| Battery        | BQ27220 fuel-gauge (I²C, addr 0x55)                                           | ✔   | —        |
| Charger / PMIC | BQ25896 + TPS65185                                                            | ✔   | TPS65185 only |
| RTC            | PCF85063 (unused)                                                             | ✔   | ✔        |
| Frontlight     | GPIO 11 via LEDC ch4 (20 kHz PWM, 8-bit)                                      | ✔   | —        |
| LoRa           | SX1262 — held in reset, firmware-disabled                                     | ✔   | —        |
| GPS            | MIA-M10Q / L76K — UART unopened, firmware-disabled                            | ✔   | —        |

Wire and epdiy share I²C-0 via `tools/patch_epdiy.py` (PlatformIO pre-build script). `main.cpp` calls `Wire.begin(39, 40)` before `display_init()`; the patched epdiy reuses Wire's driver instead of reinstalling its own.

---

## Features

**Reading.** EPUB parser with chapter navigation + on-the-fly pagination. 5 font sizes (XS/S/M/M-L/L) × 5 reader families — Sans (Lexend Deca), Serif (Literata), Slab (Bitter), ChareInk7SP, and Inter. UI chrome (menus, library, settings) always renders in Inter regardless of the reader-font choice. All five families are SIL OFL — see `LICENSES/`. 5 line-spacing presets. Partial refresh on page turn with periodic full-cleanup cadence to suppress ghosting. Bookmarks, progress bar, page/chapter indicators, swipe + tap navigation.

**Library.** List + poster view, ALL/NEW/READING/DONE filter tabs, Continue Reading banner, per-book progress badges, optional cover-art rendering in poster mode. Books are scanned from `/books`, with metadata cached in `.library_cache.json` (versioned envelope, backward-compatible loader).

**Power.** Deep sleep on inactivity (1–240 min, configurable). ext1 wake on BOOT. Wake-banner before UI redraw. Sleep-image rotation from `/sleep` with built-in fallback. WiFi flows defer the sleep timer so a long upload session is never killed mid-transfer.

**Network.** On-device WiFi scanner + QWERTY on-screen keyboard for first-time setup (works without flashing credentials). **Paperloom** browser UI for upload / download / mkdir / delete across the SD card, plus a Settings panel that syncs all device options. OTA firmware update via signed-in GitHub releases.

---

## Reader Fonts

Five reader families, all SIL OFL 1.1. Each was picked for legibility on 4-bit grayscale e-ink: uniform stroke weights, open counters, and shapes that stay crisp under partial-refresh ghosting. Full license texts live in [`LICENSES/`](LICENSES/).

- **[Lexend Deca](https://fonts.google.com/specimen/Lexend+Deca)** (Sans) — research-backed sans-serif designed to improve reading fluency by reducing visual crowding. Default reader face; pairs well with the smaller XS / S sizes.
- **[Literata](https://fonts.google.com/specimen/Literata)** (Serif) — Google Fonts' contemporary book serif (originally designed for Google Play Books). Calm rhythm and generous x-height keep long-form prose comfortable at M and M-L.
- **[Bitter](https://fonts.google.com/specimen/Bitter)** (Slab) — slab serif tuned for digital screens. The consistent stroke weight renders particularly well on e-ink and holds contrast at the larger L size.
- **[Inter](https://fonts.google.com/specimen/Inter)** (UI + reader) — also drives all UI chrome (menus, library, settings, on-screen keyboard, Paperloom web UI). Stays sharp at the small sizes the chrome uses, and is reader-selectable for readers who prefer a unified type system across UI and body text.
- **ChareInk7SP** (Reader) — SIL Charis derivative re-tuned for e-ink contrast (see the CrossInk project). Wide x-height and strong ink-density on partial-refresh make it especially easy on the eyes for long sessions.

Sizes: **XS / S / M / M-L / L** across all five families. Line-spacing: 5 presets, independent of font choice. Switch font in **Settings → Reading → Font Family** — changes apply on the next refresh.

---

## Web UI (Paperloom)

Access at `http://<device-ip>/` (HTTP only, no HTTPS — see Security below).

| Endpoint              | Method | Purpose                                                                          |
| --------------------- | ------ | -------------------------------------------------------------------------------- |
| `/`                   | GET    | HTML+CSS+JS UI (served from PROGMEM via `send_P`)                                |
| `/api/list?path=`     | GET    | Directory listing (capped at 500 items)                                          |
| `/api/mkdir`          | POST   | Create folder `{path, name}`                                                     |
| `/api/delete`         | POST   | Remove file or empty folder `{path}`                                             |
| `/api/download?path=` | GET    | Stream file with sanitized `Content-Disposition`                                 |
| `/api/upload?path=`   | POST   | Multipart upload, capped 200 MB, written to `.tmp` first then renamed atomically |
| `/api/settings`       | GET    | All settings except WiFi password                                                |
| `/api/settings`       | POST   | Update settings (validated against bounds + schema)                              |

**Hardening:**

- Path-segment guard rejects `..`, leading-dot segments (so `.settings.json` etc. are inaccessible), backslash, NUL, CR/LF, and quotes.
- Filename sanitizer for `Content-Disposition` headers blocks CRLF injection.
- Upload uses `<path>.tmp` + atomic rename so a power loss / disconnect mid-stream never leaves a corrupt file with the real name.
- Body size capped before deserialization (2 KB for settings POST, 64 KB for OTA API responses).
- Independent byte counter on uploads catches chunked-transfer payloads bigger than the cap.

---

## Security & Hardening

| Area                        | Status                                                                                           |
| --------------------------- | ------------------------------------------------------------------------------------------------ |
| HTTPS for OTA (cert-pinned) | ✅ DigiCert Global Root CA (expires 2031-11-10)                                                  |
| OTA URL allow-list          | ✅ `github.com` + `objects.githubusercontent.com` + `github-releases.githubusercontent.com` only |
| OTA partition-size guard    | ✅ Stream loop refuses to overflow update partition                                              |
| OTA response cap            | ✅ 64 KB on the API JSON body                                                                    |
| Path traversal blocked      | ✅ `..`, leading-dot segments, backslash, NUL, CR/LF, quote                                      |
| Web UI authentication       | ❌ By design — LAN-trusted; do not expose to public WiFi                                         |
| WiFi password on SD         | ⚠️ Plaintext (web UI blocks read access; physical card removal still exposes it)                 |
| Settings JSON schema        | ✅ `schemaVersion=2`; refuses to load schemas newer than what the firmware knows                 |
| Atomic SD writes            | ✅ `fsync(file)` + `fsync(parent dir)` around `rename()`; copy-fallback also synced              |
| Boot-time GC                | ✅ Removes orphan `.tmp` files in `/books` and `/books/.progress` on every boot                  |
| EPUB parser bounds          | ✅ ZIP central directory ≤ 4 MB cap, per-entry ≤ 16 MB cap, all `fread` lengths checked          |

**Threat model.** The device is intended for trusted home WiFi. The web UI is unauthenticated by design — anyone on the LAN can read, upload, and delete SD content. OTA HTTPS is pinned to a single Mozilla root, so a LAN-level MITM with a forged cert won't ship malicious firmware unless they also break DigiCert. Physical SD removal still reads the WiFi password in cleartext.

---

## Controls

### Physical buttons

| Button            | Wiring                                | Role                                                                     | Configurable?                |
| ----------------- | ------------------------------------- | ------------------------------------------------------------------------ | ---------------------------- |
| RST               | ESP32 `EN`                            | Hard chip reset                                                          | No (hardware)                |
| PWR               | Battery / charger switch              | Power on/off                                                             | No (not visible to firmware) |
| BOOT              | GPIO 0                                | Short = next page, double = prev page, long ≥ 600 ms = sleep + ext1 wake | Planned                      |
| Side (silk: IO48) | PCA9535 IO12 (I²C 0x20, port 1 bit 2) | Tap / double-tap / hold — user-mapped                                    | Yes (Settings → Device)      |

Side button: epdiy initializes IO12 as output for the TPS65185; firmware re-configures it to input on first use and samples at ~30 Hz. If the pin stays LOW for the first second after enable, polling auto-disables (treated as "not wired"). Actions per gesture: `None`, `Light toggle`, `Library`, `Sleep`, `Next page`, `Prev page`, `Menu`.

The double-tap window is **250 ms** (down from 400 ms in earlier firmware).

### Touch

**Library.** Tap a book to open. Tap footer for Settings. Tap header to cycle sort order. Swipe ←/→ to page through the list. Tap Continue Reading banner to resume the last book.

**Reader.** Left third = prev page. Right third = next page. Center third = menu overlay. Long-press center = bookmark. Swipe ←/→ to page (move-tracking enabled — small drift on a tap won't trigger).

**Reader menu.** TOC, Bookmarks, Settings, Library. Tap outside the menu items to resume reading.

**Keyboard.** Full QWERTY 4×10 grid plus a function row (`123` mode toggle, Space, Cancel, Done). Used in the on-device WiFi setup. Password fields show only the most recent character; previous characters render as `*`.

---

## SD Card Layout

```text
/
├── books/
│   ├── *.epub
│   ├── .library_cache.json          # versioned envelope {cacheVersion, entries}
│   ├── .settings.json               # schemaVersion=2
│   ├── .progress/<book>.json        # one per book
│   └── .linecache/
│       ├── ch<chapter>.txt          # word-wrapped lines, indexed by offset
│       └── inline/
│           ├── img_<hash>.<png|jpg|jpeg>
│           └── raw4_<hash>_<w>x<h>.r4
└── sleep/*.{png,jpg,jpeg}
```

Folders are auto-created on first successful SD mount. Orphan `.tmp` files (left by interrupted writes) are removed at boot. The `.linecache/inline` raw4 files store one 4-bit grayscale value per byte (unpacked) so the reader streams directly from SD without live JPEG / PNG decode during a page turn.

---

## Behavior Notes

### Sleep images

Drop PNG / JPG / JPEG files into `/sleep` (or upload via Paperloom). The firmware picks the next image in sorted-filename order, decodes, scales-to-fit, converts to grayscale, and centers it. Files larger than ~8 MB are rejected; failed decodes fall through to the next image. If none are usable, the built-in fallback screen is shown.

Recommended source size: **540 × 960** portrait.

### Inline EPUB images

JPEG / JPG inline images render via an SD-backed pre-rendered raw4 cache (no live decode during page draw). **PNG inline images are skipped** — the current PNG raw4 path corrupts heap on ESP32-S3 and is pending a fix.

### OTA updates

**Settings → Firmware Update** → WiFi connects → fetches latest GitHub release JSON → version compare → installs the `firmware.bin` asset if newer → reboot.

Release side: run `python3 tools/build.py X.Y.Z` to build env `gh_release`, then attach `.pio/build/gh_release/firmware.bin` to the GitHub release. The on-device checker expects that exact filename.

If OTA finds nothing: verify WiFi credentials, that a newer release tag exists, and that the release contains a `firmware.bin` asset.

### Boot path

Cold boot → splash → SD mount → orphan-`.tmp` GC → settings load (schema-checked) → library scan → library screen. Wake from deep sleep → wake banner → restore reader (only if the previous book reopens cleanly) or fall back to library. A "Previous session crashed — book position reset" banner overlays the library on the next boot if the crash guard tripped.

---

## Performance Tuning Applied

- DEFLATE chapter buffer in **PSRAM** (was DRAM) — large chapters no longer threaten the 320 KB internal heap.
- Library scan EOCD search buffer in **PSRAM** — 100-book cold scans no longer fragment DRAM.
- `display_draw_filled_rect` uses **memset over packed nibbles** — full-screen fills run ~100× faster than the previous per-pixel `pset()` loop.
- OTA download chunks at **16 KB** to match the TLS record size — roughly halves wall-clock time vs. 4 KB.
- Frontlight LEDC at **20 kHz** — eliminates a periodic luminance coupling artifact that occurred at the previous 5 kHz carrier.
- Boot path drops the unconditional 500 ms serial-settle delay.
- `library_cache.json` allocation is now sized from the actual entry count (was a fixed 8 KB that silently truncated past ~30 books).
- Library cache load is gated by `ESP.getFreeHeap()` — refuses to allocate the 48 KB document under heap pressure.
- `library_scan` feeds the watchdog between cache-miss EPUB opens.

---

## Known Limitations

- **No web authentication** — by design for LAN use only. Do not bridge to public WiFi.
- **WiFi password plaintext on SD** — physical card removal exposes it. The web UI cannot read it.
- **Single OTA root cert** — DigiCert Global Root CA. When GitHub rotates anchors, OTA will silently start failing instead of being insecure; update `DIGICERT_GLOBAL_ROOT_CA` in `src/ota_update.cpp`.
- **Text-first EPUB rendering** — heavy CSS layouts will not reproduce perfectly. PNG inline images are skipped (heap corruption pending fix).
- **True power-off needs the hardware PWR switch**; the BOOT button only triggers deep sleep.
- **Roadmap** (not yet implemented): OPDS catalog client, Project Gutenberg genre browser, encrypted WiFi credential storage.

---

## Documentation

- `DESIGN.md` — UI conventions (footer style, touch zones), AppState transitions, WiFi flow.
- `CHANGELOG.md` — version history.
- `include/config.h.example` — pin map + WiFi placeholder values.

---

## License

MIT

# Paperloom

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange)](https://platformio.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A minimalist EPUB reader firmware for two LilyGo e-ink boards. Built for people who just want to read books on a 4.7" e-paper display without ads, tracking, or cloud accounts.

Supported hardware:

- [**LilyGo T5 E-Paper S3 Pro**](https://lilygo.cc/products/t5-e-paper-s3-pro) — with touch, frontlight, side button, battery gauge.
- [**LilyGo T5 E-Paper S3 Pro Lite**](https://lilygo.cc/products/t5-e-paper-s3-pro-lite) — same screen, lighter peripherals.

Version: **v0.2.1**

---

## What it does

- Reads EPUB books from a microSD card
- Five reader fonts, five text sizes, five line-spacing presets
- Library view with covers, reading progress, and Continue Reading
- Touch + side button + BOOT button — all gestures user-mappable
- Built-in web uploader — drop books in a browser, no cable needed
- WiFi setup on-device with an on-screen keyboard
- Over-the-air firmware updates from GitHub releases
- Sleep-image gallery from your own pictures
- Bookmarks, chapter navigation, swipe pages

---

## Getting started

### 1. Prepare an SD card

- Format as **FAT32** (any size).
- Optional: copy `.epub` files into `/books`, sleep images into `/sleep`.

### 2. Flash the firmware

You have two options.

**Option A — Browser installer (no tools required).**

Open **[lazybone.github.io/Paperloom](https://lazybone.github.io/Paperloom/)** in **Chrome, Edge, or Opera** on a desktop computer. Plug the e-reader in over USB-C and click *Install*. The page uses the browser's Web Serial API to flash the firmware directly — no PlatformIO, no esptool, no command line.

**Option B — PlatformIO (for developers).**

```bash
cp include/config.h.example include/config.h
pio run -t upload
```

### 3. First boot

1. Insert SD card, power on.
2. Open **Settings → WiFi Setup**. Pick your network, type the password on the on-screen keyboard.
3. Open **Settings → WiFi Upload**. The screen shows an IP address like `http://192.168.1.42/`.
4. Open that address in any browser on the same network — drag-and-drop EPUBs.

---

## Reading

**Touch zones in a book:**

| Tap where        | What happens         |
| ---------------- | -------------------- |
| Left third       | Previous page        |
| Right third      | Next page            |
| Center third     | Open menu            |
| Long-press center | Add bookmark         |
| Swipe ←/→        | Page forward / back  |

**Library:**

- Tap a book to open it
- Tap header to change sort order
- Tap footer for Settings
- The Continue Reading banner jumps back into the last book you read

**Buttons:**

Both the **BOOT button** (on the case) and the **side button** (silk-screened IO48) can be mapped to whatever you want — tap, double-tap, and hold each have their own action:

- Toggle frontlight
- Open Library
- Sleep
- Next / Previous page
- Open menu
- Nothing

Set them up in **Settings → Device**.

---

## The web uploader

When you turn on **Settings → WiFi Upload**, the device starts a small web server on your home network. Open the displayed address in a browser to:

- Upload books (up to 200 MB per file)
- Download or delete books
- Make folders
- Change every setting the device offers

The web page also runs without internet — it's served directly from the device.

> **Tip:** Sleep images go in `/sleep`. Portrait pictures around **540 × 960** look best.

---

## Updating the firmware

**Settings → Firmware Update** — the device checks GitHub for a newer release and installs it over WiFi. No cable, no PlatformIO needed after the first flash.

---

## Things to know

- **Use it on your home WiFi only.** The web uploader has no password — anyone on the same network can read or change the SD card. Don't use it on public hotspots.
- **Your WiFi password is stored on the SD card in plain text.** Anyone with physical access to the card can read it.
- **PNG images inside books are skipped right now.** JPEGs work. Plain text always works.
- **The PWR switch is the only true power-off.** All on-screen "sleep" options are deep sleep, not power-down.
- **Heavy CSS layouts won't render perfectly.** Paperloom is a text-first reader.

---

## SD card layout

You normally never need to touch this — Paperloom creates everything itself. For reference:

```text
/
├── books/
│   ├── *.epub                 ← your books go here
│   └── (hidden cache files)
└── sleep/
    └── *.png, *.jpg           ← sleep-screen pictures
```

---

## More documentation

- [`DESIGN.md`](DESIGN.md) — UI conventions, screen layouts
- [`CHANGELOG.md`](CHANGELOG.md) — what changed in each version
- [`include/config.h.example`](include/config.h.example) — compile-time options for developers

---

## License

MIT — see [`LICENSE`](LICENSE). Bundled reader fonts are SIL OFL 1.1, see [`LICENSES/`](LICENSES/).

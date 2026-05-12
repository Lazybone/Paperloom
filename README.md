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

#### Option A — Browser installer (recommended, no tools required)

The fastest way. Works on Windows, macOS, Linux, and ChromeOS — no drivers, no PlatformIO, no command line.

1. Open **[lazybone.github.io/Paperloom](https://lazybone.github.io/Paperloom/)** in **Google Chrome**, **Microsoft Edge**, or **Opera** on a desktop computer.
   _(Firefox and Safari are not supported — they don't implement Web Serial.)_
2. Plug the e-reader into your computer with a **USB-C cable** that supports data (not just charging).
3. On the device, hold the **BOOT** button while pressing **RST** once, then release BOOT. This forces download mode.
4. On the web page, click **Connect** → pick the serial port that appears (usually labeled `USB JTAG/serial debug unit` or similar).
5. Click **Install Paperloom** → wait ~30–60 seconds for the flash to complete.
6. When it's done, press **RST** once. The device boots into Paperloom.

If no port shows up, try a different USB-C cable — many cables are power-only and silently won't enumerate as a serial device.

On Linux, make sure your user is in the `dialout` group:

```bash
sudo usermod -aG dialout $USER   # log out and back in afterwards
```

#### Option B — PlatformIO (for developers)

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

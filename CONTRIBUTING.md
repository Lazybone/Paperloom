# Contributing to Paperloom

Thanks for your interest in improving Paperloom. This document covers the
practical bits you need to send a pull request that lands cleanly.

## Project layout

```
src/          Firmware sources (.cpp), UI screens under src/ui/
include/      Public headers + config.h.example
boards/       PlatformIO board JSON for LilyGo T5S3-4.7" PRO
tools/        Build / flash / asset helpers (Python)
LICENSES/     Third-party font + asset licenses
partitions.csv  Flash layout (OTA dual-slot)
platformio.ini  Build configuration
```

## Development setup

Prereqs:

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
  (`pio` on `$PATH`)
- USB-C cable
- LilyGo T5S3-4.7" e-paper PRO (or PRO Lite — both share the same firmware)
- A FAT32 microSD card

First-time setup:

```bash
git clone https://github.com/Lazybone/Paperloom.git
cd Paperloom
cp include/config.h.example include/config.h   # template; do NOT commit your edits
pio run                                         # build default env
pio run -t upload                               # build + flash
```

`include/config.h` is gitignored. Keep WiFi credentials and any local secrets
only in your local copy.

## Build environments

- `default` — development build, `CORE_DEBUG_LEVEL=3`
- `gh_release` — release build, `CORE_DEBUG_LEVEL=1`, consumes `RELEASE_TAG`

```bash
pio run -e default
pio run -e gh_release
```

Cutting a versioned release is automated:

```bash
python3 tools/build.py 0.2.0          # bump + build
python3 tools/build.py 0.2.0 --publish   # also gh release create
```

## Coding style

- **C++ standard:** the toolchain default for PlatformIO Arduino-ESP32.
  Modern idioms (RAII, `nullptr`, range-for) preferred, but watch heap use —
  this is a 320 KB RAM / 8 MB PSRAM device.
- **Naming:** `snake_case` for files, functions, and local variables;
  `PascalCase` for types and classes; `UPPER_SNAKE_CASE` for constants and
  macros. Match the surrounding file if in doubt.
- **Headers:** every public header uses `#pragma once`. Keep declarations in
  `include/` or `src/<feature>.h`; implementations in `src/<feature>.cpp`.
- **No raw `strcpy` / `strcat` / `sprintf`** on fixed buffers. Use
  `snprintf(buf, sizeof(buf), ...)` or `String` and check return values.
- **Allocations:** prefer stack or pre-sized buffers. Use `ps_malloc` /
  `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` when an allocation belongs in
  PSRAM. Check every allocation result.
- **Logging:** gate verbose logs behind `CORE_DEBUG_LEVEL` or a feature-
  specific `DBG_*` macro. Production builds should be quiet.

## Commit messages

Follow Conventional Commits:

```
<type>: <short summary, imperative>

<optional body explaining the why>
```

Common types: `feat`, `fix`, `refactor`, `docs`, `test`, `chore`, `perf`,
`ci`, `build`.

Reference issues when applicable (`Refs #42`, `Fixes #42`).

## Pull requests

1. Branch from `main`: `git checkout -b feat/<short-name>` or
   `fix/<short-name>`.
2. Keep the diff focused. Split unrelated changes into separate PRs.
3. Verify the firmware still builds on both envs before pushing:
   ```bash
   pio run -e default
   pio run -e gh_release   # if you touch anything release-relevant
   ```
4. If your change affects on-device behavior, test on real hardware and
   describe what you ran in the PR body. The CI build alone is not enough.
5. If the change is user-visible, update `CHANGELOG.md` under the
   `## Unreleased` section.
6. Fill in the PR template — especially the hardware variant, firmware
   version under test, and a short test plan.

## Reporting bugs

Use the bug report issue template. Include:

- Hardware variant (PRO or PRO Lite)
- Firmware version (`Settings → About` or the value of `FIRMWARE_VERSION`)
- Reproduction steps
- Expected vs. observed behavior
- Serial log if you have one (`pio device monitor` at 115200 baud)

## Reporting security issues

Do **not** open a public issue for security vulnerabilities. See
[`SECURITY.md`](SECURITY.md) for the responsible disclosure process.

## License

By contributing you agree that your contributions are licensed under the
MIT License (see [`LICENSE`](LICENSE)).

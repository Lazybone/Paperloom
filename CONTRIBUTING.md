# Contributing to Paperloom

Thanks for your interest in improving Paperloom! This guide covers everything you need to get started.

## Development Setup

**Prerequisites**

- [PlatformIO Core](https://platformio.org/install/cli) (CLI version)
- USB-C data cable
- FAT32-formatted microSD card
- LilyGo T5 E-Paper S3 **Pro** or **Pro Lite**

**Quick start**

```bash
git clone <repo-url>
cd paperloom
cp include/config.h.example include/config.h
pio run -t upload          # build + flash
pio device monitor -b 115200
```

Use `python tools/flash.py` as a one-shot wrapper for the common build/flash/monitor cycle.

## Build Variants

| Environment | Purpose | Command |
|-------------|---------|---------|
| `default` | Daily development, verbose logs (`CORE_DEBUG_LEVEL=3`) | `pio run` |
| `gh_release` | Release builds (`CORE_DEBUG_LEVEL=1`) | `pio run -e gh_release` |

Release builds consume the `RELEASE_TAG` environment variable for OTA version strings.

## Style Guide

- **Files / functions / variables**: `snake_case`
- **Headers**: `#pragma once` (no include guards)
- **Braces**: K&R / 1TBS — opening brace on the same line
- **Indent**: 4 spaces (no tabs)
- **Line length**: aim for ≤100 characters
- **Comments**: `//` for inline, `/* */` only for block headers
- **C++ standard**: C++17 (ESP32 Arduino)

## Commit Messages

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <short description>

<body — what and why, not how>
```

Common types: `feat`, `fix`, `refactor`, `docs`, `chore`, `test`

Example:
```
fix(ui): prevent dots buffer overflow in WiFi status

Replace unbounded strcat loop with snprintf + explicit
bounds check. connectDots is mod-4 but the guard makes
the code safe against future refactoring.
```

## Pull Request Checklist

- [ ] PR targets `main`
- [ ] Code compiles for both `default` and `gh_release` envs
- [ ] No new compiler warnings (`-Wall -Wextra` clean)
- [ ] Relevant sections of README updated if user-facing
- [ ] CHANGELOG.md updated under `## Unreleased` if applicable
- [ ] If adding a feature: screenshot or photo of UI change attached

## Testing

There is currently no automated test suite. Please:

1. Flash to **both** Pro and Pro Lite hardware if your change touches display, touch, or peripheral code.
2. Verify the SD-card path still works after any filesystem change.
3. Check OTA and WiFi upload paths after network or security changes.

## Questions?

Open a [GitHub Discussion](https://github.com/anomalyco/Paperloom/discussions) or reach out via the contact in [SECURITY.md](SECURITY.md).

# Security Policy

Paperloom is an internet-connected device firmware (WiFi + OTA updates) for the
LilyGo T5S3-4.7" e-paper PRO / PRO Lite. We take security reports seriously and
ask reporters to follow coordinated disclosure.

## Supported Versions

Only the latest released firmware version receives security fixes. Older
versions are not patched; please upgrade.

| Version | Supported |
| ------- | --------- |
| v0.2.x (latest) | ✓ |
| < v0.2.0        | ✗ |

## Reporting a Vulnerability

**Please do not open public GitHub issues for security problems.**

Report privately via **GitHub Security Advisories**:

➡ https://github.com/Lazybone/Paperloom/security/advisories/new

When reporting, please include:

- Affected firmware version (`Settings → About` on device, or `firmware.bin` SHA)
- Hardware variant (Pro vs Pro Lite)
- Reproduction steps or a proof-of-concept
- Impact assessment (RCE, info disclosure, DoS, persistent vs reset-on-reboot, …)
- Any suggested mitigation

## What to Expect

- **Acknowledgement**: within 7 days of report.
- **Initial assessment**: within 14 days.
- **Embargo period**: up to **90 days** for coordinated disclosure. We may
  request a shorter or longer window depending on severity, exploitability,
  and upstream-dependency complexity.
- **Credit**: reporters are credited in the release notes and advisory unless
  they request anonymity.

## Scope

In scope:

- Firmware code in this repository (OTA client, WiFi captive setup, web upload
  endpoint, EPUB/ZIP parser, settings persistence)
- Build artefacts published via GitHub Releases
- Vulnerabilities affecting confidentiality, integrity, or availability of the
  device or the data it stores (EPUB library, WiFi credentials, settings)

Out of scope:

- Physical attacks (chip decapping, JTAG with hardware access) — these are
  acknowledged as inherent to a consumer e-paper device and are not in the
  threat model
- Vulnerabilities in upstream libraries (`epdiy`, `ArduinoJson`, `JPEGDEC`,
  `PNGdec`, `tinyxml2`, ESP-IDF) — please report those to their respective
  maintainers, but feel free to also CC us so we can coordinate a pin/upgrade.
- Reports based solely on missing security headers in the unauthenticated LAN
  web upload UI — the README documents the LAN-only threat model.

## Hall of Fame

Reporters who follow coordinated disclosure will be listed here.

<!-- empty until first report -->

# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| v0.2.x  | ✓ Yes     |
| < v0.2.0| ✗ No      |

Security fixes are back-ported to the latest minor release only. Please keep your device up to date via the on-device OTA updater or by re-flashing the latest release.

## Reporting a Vulnerability

**Please do NOT open a public GitHub issue for security vulnerabilities.**

Instead, please use **GitHub Security Advisories** to report vulnerabilities privately:

1. Go to the repository's **Security** tab
2. Click **Report a vulnerability**
3. Fill out the advisory form with as much detail as possible

Please include:

- Firmware version (shown on boot screen or in Settings → About)
- Hardware variant (Pro / Pro Lite)
- Step-by-step reproduction instructions
- Impact assessment (crash, data leak, remote code execution, etc.)
- Suggested fix or patch, if you have one

## Response Timeline

| Phase | Target |
|-------|--------|
| Acknowledgement | Within 48 hours |
| Initial assessment | Within 5 days |
| Fix + pre-release test | Within 30 days (critical: 7 days) |
| Public disclosure | Coordinated with reporter; 90-day embargo default |

We follow a **coordinated disclosure** model. We will not publish details or push a fix before the agreed embargo date unless the vulnerability is already publicly known or actively exploited.

## Security Hardening Notes

### Transport & Updates
- OTA updates are served **only** from GitHub releases over **HTTPS**. The firmware pins a small bundle of trusted GitHub root CAs (USERTrust ECC for `api.github.com`, ISRG Root X1 for the release CDN, plus DigiCert Global Root CA as a historical fallback). The Arduino-ESP32 default TLS path otherwise has no trust store, so the bundle is the only thing standing between the OTA flow and MITM.
- Download URLs are constrained to `objects.githubusercontent.com` / `github.com` release paths after redirect resolution.
- No remote administration interface exists beyond the LAN-local web UI.

### Web UI (`Paperloom`)
- All filesystem paths are validated before use: must start with `/`, no `..` segments, no backslash, no `CR`/`LF` (prevents HTTP header smuggling), no `NUL` bytes, and no path segment beginning with `.` — this isolates internal state files (`.settings.json`, `.progress`, `.linecache`) from being read, written, or deleted via the API.
- Uploads use atomic `.tmp` + rename to avoid partial-write corruption, and are capped at 200 MB.
- Filenames are restricted to a safe character set; oversize names are rejected.
- The settings API never returns the stored WiFi password. The client sends an empty `wifiPass` field to keep the existing password and only sends a new value when the user enters one.
- Input lengths are bounded server-side (`wifiSSID` ≤ 32, `wifiPass` ≤ 64, numeric ranges clamped).

### Memory Safety
- Recently audited paths use `snprintf`-style bounded writes instead of `strcat`/`sprintf` loops; remaining fixed-size buffers carry explicit length guards.

### Credential Storage
- WiFi credentials are stored as plaintext JSON in `/.settings.json` on the SD card. The dot-prefix hides the file from the web file browser and the path validator blocks remote access, but **anyone with physical SD-card access can read the password**. Treat the SD card as a credential-bearing asset.
- A hardware-backed encrypted store (NVS with flash encryption, or a dedicated key in eFuse) is on the roadmap. Contributions welcome — see `CONTRIBUTING.md`.

## Acknowledgements

We credit security researchers who report valid vulnerabilities in the release notes (with their permission).

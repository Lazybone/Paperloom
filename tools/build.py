#!/usr/bin/env python3
"""Build a release-ready firmware binary, rename it for the GitHub release,
roll the CHANGELOG `## Unreleased` block into a versioned section, and emit a
matching release-notes file that can be pasted (or `gh release create -F`'d)
into the GitHub release body.

Usage:
    python3 tools/build.py 0.2.0
    python3 tools/build.py 0.2.0 --product Paperloom --date 2026-05-12
    python3 tools/build.py 0.2.0 --skip-changelog        # binary only
    python3 tools/build.py 0.2.0 --dry-run               # show plan, do nothing

Outputs (in ./build/):
    Paperloom-<version>.bin             — flashable firmware image
    release_notes_<version>.md          — markdown body for the GitHub release
"""

from __future__ import annotations

import argparse
import datetime as _dt
import glob
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CHANGELOG = REPO_ROOT / "CHANGELOG.md"
CONFIG_H = REPO_ROOT / "include" / "config.h"
README = REPO_ROOT / "README.md"
BUILD_DIR = REPO_ROOT / "build"
MANIFEST = REPO_ROOT / "docs" / "manifest.json"
PIO_BUILD_DIR = REPO_ROOT / ".pio" / "build" / "gh_release"
PIO_BIN = PIO_BUILD_DIR / "firmware.bin"
PIO_BOOTLOADER = PIO_BUILD_DIR / "bootloader.bin"
PIO_PARTITIONS = PIO_BUILD_DIR / "partitions.bin"
PIO_ENV = "gh_release"

# ESP32-S3 standard flash offsets used by the Arduino framework.
FLASH_OFFSET_BOOTLOADER = 0x0000
FLASH_OFFSET_PARTITIONS = 0x8000
FLASH_OFFSET_BOOT_APP0 = 0xE000
FLASH_OFFSET_APP = 0x10000

# Matches `#define FIRMWARE_VERSION "x.y.z"` regardless of inner whitespace.
# The gh_release PlatformIO env overrides this via -D so the in-source value is
# only consumed by the `default` env build, but we still bump it so a local
# `pio run` (no RELEASE_TAG) prints a sane version on the device.
CONFIG_VERSION_RE = re.compile(
    r'(#define\s+FIRMWARE_VERSION\s+)"[^"]+"'
)

# Matches the top-of-README badge line:  Version: **v0.1.0** · PlatformIO env: `default`
# Captures the markdown wrapper around the version token so we replace the
# semver only and leave bold/italic formatting and the rest of the line intact.
README_VERSION_RE = re.compile(
    r'(Version:\s*\*\*v)(\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)(\*\*)'
)

SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")
UNRELEASED_HEADER = "## Unreleased"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("version", help="Semantic version without leading v, e.g. 0.2.0")
    p.add_argument("--product", default="Paperloom", help="Binary name prefix (default: Paperloom)")
    p.add_argument(
        "--date",
        default=_dt.date.today().isoformat(),
        help="Release date for CHANGELOG header (default: today)",
    )
    p.add_argument("--skip-build", action="store_true", help="Reuse the existing firmware.bin from .pio")
    p.add_argument("--skip-changelog", action="store_true", help="Do not rewrite CHANGELOG.md")
    p.add_argument("--skip-version-bump", action="store_true",
                   help="Do not edit FIRMWARE_VERSION in include/config.h")
    p.add_argument("--publish", action="store_true",
                   help="After building, create the GitHub release via `gh release create` and "
                        "attach Paperloom-<version>.bin + firmware.bin")
    p.add_argument("--draft", action="store_true",
                   help="Publish as draft (only meaningful with --publish)")
    p.add_argument("--repo", default=None,
                   help="GitHub repo for --publish (e.g. Lazybone/Paperloom). "
                        "Defaults to the repo detected from the local git remote.")
    p.add_argument("--dry-run", action="store_true", help="Print planned actions, do not execute")
    return p.parse_args()


def fail(msg: str) -> "None":
    print(f"build.py: error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def validate_version(version: str) -> None:
    if not SEMVER_RE.match(version):
        fail(f"version '{version}' is not semver (e.g. 0.2.0 or 0.2.0-rc1)")


def split_changelog(text: str) -> tuple[str, str, str]:
    """Return (preamble, unreleased_body, rest) where:
        preamble        = everything up to and including the `## Unreleased` line + the blank line under it
        unreleased_body = lines belonging to the Unreleased section (no trailing blanks)
        rest            = remaining versioned sections starting at the next `## ` heading
    """
    lines = text.splitlines(keepends=True)
    try:
        idx = next(i for i, line in enumerate(lines) if line.strip() == UNRELEASED_HEADER)
    except StopIteration:
        fail("CHANGELOG.md has no '## Unreleased' section to release from")

    body_start = idx + 1
    body_end = body_start
    while body_end < len(lines) and not lines[body_end].startswith("## "):
        body_end += 1

    preamble = "".join(lines[: idx + 1])
    body = "".join(lines[body_start:body_end]).strip("\n")
    rest = "".join(lines[body_end:])
    return preamble, body, rest


def bump_config_version(version: str, dry_run: bool) -> None:
    if not CONFIG_H.is_file():
        fail(f"missing {CONFIG_H}")
    text = CONFIG_H.read_text(encoding="utf-8")
    new_text, count = CONFIG_VERSION_RE.subn(
        lambda m: f'{m.group(1)}"{version}"', text, count=1,
    )
    if count == 0:
        fail(f"could not find '#define FIRMWARE_VERSION \"...\"' in {CONFIG_H}")
    if new_text == text:
        print(f"-> {CONFIG_H.relative_to(REPO_ROOT)}: already at {version}")
        return
    print(f"-> {CONFIG_H.relative_to(REPO_ROOT)}: FIRMWARE_VERSION -> \"{version}\"")
    if dry_run:
        return
    CONFIG_H.write_text(new_text, encoding="utf-8")


def bump_readme_version(version: str, dry_run: bool) -> None:
    if not README.is_file():
        print(f"-> {README.name}: missing, skipping")
        return
    text = README.read_text(encoding="utf-8")
    new_text, count = README_VERSION_RE.subn(
        lambda m: f"{m.group(1)}{version}{m.group(3)}", text, count=1,
    )
    if count == 0:
        print(f"-> {README.relative_to(REPO_ROOT)}: no 'Version: **vX.Y.Z**' line, skipping")
        return
    if new_text == text:
        print(f"-> {README.relative_to(REPO_ROOT)}: already at v{version}")
        return
    print(f"-> {README.relative_to(REPO_ROOT)}: Version -> v{version}")
    if dry_run:
        return
    README.write_text(new_text, encoding="utf-8")


def build_firmware(version: str, dry_run: bool) -> None:
    env = os.environ.copy()
    env["RELEASE_TAG"] = f"v{version}"
    cmd = ["pio", "run", "-e", PIO_ENV]
    print(f"-> RELEASE_TAG={env['RELEASE_TAG']} {' '.join(cmd)}")
    if dry_run:
        return
    result = subprocess.run(cmd, cwd=REPO_ROOT, env=env)
    if result.returncode != 0:
        fail(f"`pio run -e {PIO_ENV}` exited with status {result.returncode}")


def copy_binary(out_paths: list[Path], dry_run: bool) -> None:
    for out_path in out_paths:
        print(f"-> {PIO_BIN.relative_to(REPO_ROOT)}  ->  {out_path.relative_to(REPO_ROOT)}")
    if dry_run:
        return
    if not PIO_BIN.is_file():
        fail(f"expected firmware at {PIO_BIN} — build did not produce it")
    for out_path in out_paths:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(PIO_BIN, out_path)


def find_esptool() -> "list[str] | None":
    """Return a command prefix that invokes esptool, or None if it cannot be located.

    Search order: PATH (`esptool.py`, `esptool`), `python -m esptool`, the
    PlatformIO bundled copy under ~/.platformio/packages/tool-esptoolpy.
    """
    for name in ("esptool.py", "esptool"):
        path = shutil.which(name)
        if path:
            return [path]

    if subprocess.run(
        [sys.executable, "-m", "esptool", "--help"],
        capture_output=True,
    ).returncode == 0:
        return [sys.executable, "-m", "esptool"]

    matches = sorted(
        glob.glob(str(Path.home() / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py"))
    )
    if matches:
        return [sys.executable, matches[0]]

    return None


def find_boot_app0() -> "Path | None":
    """Locate boot_app0.bin from the installed Arduino-ESP32 framework."""
    pattern = str(
        Path.home() / ".platformio" / "packages" / "framework-arduinoespressif32"
        / "tools" / "partitions" / "boot_app0.bin"
    )
    matches = sorted(glob.glob(pattern))
    return Path(matches[0]) if matches else None


def build_merged_binary(out_path: Path, dry_run: bool) -> bool:
    """Produce a single flashable image suitable for esp-web-tools (offset 0x0).

    Combines bootloader, partition table, OTA-data init, and the application
    into one file. Returns True on success, False if a prerequisite is missing
    (caller decides whether that is fatal).
    """
    esptool_cmd = find_esptool()
    if esptool_cmd is None:
        print("-> merged image: skipped (esptool not found — install with `pip install esptool`)")
        return False

    boot_app0 = find_boot_app0()
    if boot_app0 is None:
        print("-> merged image: skipped (boot_app0.bin not found under ~/.platformio/packages)")
        return False

    for required in (PIO_BOOTLOADER, PIO_PARTITIONS, PIO_BIN):
        if not required.is_file() and not dry_run:
            print(f"-> merged image: skipped ({required.name} missing — was the build successful?)")
            return False

    cmd = [
        *esptool_cmd,
        "--chip", "esp32s3",
        "merge_bin",
        "--output", str(out_path),
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", "16MB",
        f"0x{FLASH_OFFSET_BOOTLOADER:x}", str(PIO_BOOTLOADER),
        f"0x{FLASH_OFFSET_PARTITIONS:x}", str(PIO_PARTITIONS),
        f"0x{FLASH_OFFSET_BOOT_APP0:x}", str(boot_app0),
        f"0x{FLASH_OFFSET_APP:x}", str(PIO_BIN),
    ]

    print(f"-> merged image:   {out_path.relative_to(REPO_ROOT)}  (esptool merge_bin)")
    if dry_run:
        return True

    out_path.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(cmd, cwd=REPO_ROOT)
    if result.returncode != 0:
        fail(f"`esptool merge_bin` exited with status {result.returncode}")
    return True


def bump_manifest_version(version: str, dry_run: bool) -> None:
    """Update the version field of docs/manifest.json (web-installer)."""
    if not MANIFEST.is_file():
        print(f"-> {MANIFEST.relative_to(REPO_ROOT)}: missing, skipping")
        return

    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if data.get("version") == version:
        print(f"-> {MANIFEST.relative_to(REPO_ROOT)}: already at {version}")
        return

    data["version"] = version
    print(f"-> {MANIFEST.relative_to(REPO_ROOT)}: version -> {version}")
    if dry_run:
        return

    MANIFEST.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def publish_github_release(
    version: str,
    bin_paths: list[Path],
    notes_path: Path,
    repo: "str | None",
    draft: bool,
    dry_run: bool,
) -> None:
    if shutil.which("gh") is None:
        fail("`gh` CLI not found in PATH — install from https://cli.github.com/ or drop --publish")

    tag = f"v{version}"
    title = f"Paperloom {tag}"
    cmd = [
        "gh", "release", "create", tag,
        "--title", title,
        "--notes-file", str(notes_path),
    ]
    if draft:
        cmd.append("--draft")
    if repo:
        cmd.extend(["--repo", repo])
    cmd.extend(str(p) for p in bin_paths)

    print(f"-> publish:        {' '.join(cmd)}")
    if dry_run:
        return

    # Verify each asset exists before we make a half-published release.
    for p in bin_paths:
        if not p.is_file():
            fail(f"asset missing: {p}")
    if not notes_path.is_file():
        fail(f"notes missing: {notes_path}")

    result = subprocess.run(cmd, cwd=REPO_ROOT)
    if result.returncode != 0:
        fail(f"`gh release create` exited with status {result.returncode}")


def write_release_notes(notes_path: Path, version: str, date: str, body: str, dry_run: bool) -> None:
    if not body.strip():
        body = "_No changelog entries recorded for this release._"
    content = (
        f"# Paperloom v{version} — {date}\n\n"
        f"{body.rstrip()}\n\n"
        f"## Install\n\n"
        f"Flash `Paperloom-{version}.bin` over USB-C with `esptool.py`, or push "
        f"this release tag to let the on-device OTA picker fetch it on the next "
        f"firmware-update check.\n"
    )
    print(f"-> release notes:  {notes_path.relative_to(REPO_ROOT)}")
    if dry_run:
        return
    notes_path.parent.mkdir(parents=True, exist_ok=True)
    notes_path.write_text(content, encoding="utf-8")


def rewrite_changelog(version: str, date: str, dry_run: bool) -> str:
    text = CHANGELOG.read_text(encoding="utf-8")
    preamble, body, rest = split_changelog(text)

    if not body.strip():
        fail("'## Unreleased' section is empty — add notes before cutting a release")

    versioned_header = f"## v{version} — {date}\n"
    new_text = (
        preamble
        + "\n"
        + versioned_header
        + "\n"
        + body.rstrip()
        + "\n\n"
        + rest.lstrip("\n")
    )

    # Tidy: ensure preamble still ends with a single blank line after the
    # `## Unreleased` header so subsequent diffs stay minimal.
    new_text = re.sub(r"\n{3,}", "\n\n", new_text)

    print(f"-> CHANGELOG.md:   add `{versioned_header.strip()}` block, reset Unreleased")
    if not dry_run:
        CHANGELOG.write_text(new_text, encoding="utf-8")
    return body


def main() -> int:
    args = parse_args()
    validate_version(args.version)

    if not CHANGELOG.is_file():
        fail(f"missing {CHANGELOG}")

    # Three binary copies on the release:
    #   <Product>-<version>.bin    — friendly download name shown on the release page
    #   firmware.bin               — exact asset name the on-device OTA checker expects
    #   Paperloom-merged.bin       — single-file image consumed by the web installer
    #                                (docs/manifest.json → esp-web-tools).
    out_bin = BUILD_DIR / f"{args.product}-{args.version}.bin"
    out_ota = BUILD_DIR / "firmware.bin"
    out_merged = BUILD_DIR / "Paperloom-merged.bin"
    out_notes = BUILD_DIR / f"release_notes_{args.version}.md"

    # We need the Unreleased body for the release notes even when --skip-changelog
    # is set, so always parse it first.
    _, unreleased_body, _ = split_changelog(CHANGELOG.read_text(encoding="utf-8"))

    if not args.skip_version_bump:
        bump_config_version(args.version, args.dry_run)
        bump_readme_version(args.version, args.dry_run)
        bump_manifest_version(args.version, args.dry_run)

    if not args.skip_build:
        build_firmware(args.version, args.dry_run)

    copy_binary([out_bin, out_ota], args.dry_run)
    merged_ok = build_merged_binary(out_merged, args.dry_run)

    body_for_notes = unreleased_body
    if not args.skip_changelog:
        body_for_notes = rewrite_changelog(args.version, args.date, args.dry_run)

    write_release_notes(out_notes, args.version, args.date, body_for_notes, args.dry_run)

    release_assets = [out_bin, out_ota]
    if merged_ok:
        release_assets.append(out_merged)

    if args.publish:
        publish_github_release(
            args.version,
            release_assets,
            out_notes,
            args.repo,
            args.draft,
            args.dry_run,
        )

    print()
    asset_names = ", ".join(f"`{p.name}`" for p in release_assets)
    if args.publish:
        kind = "draft release" if args.draft else "release"
        print(f"done.  GitHub {kind} v{args.version} created with {asset_names} attached.")
        print(f"       Make sure the version-bump commit (config.h, README.md, CHANGELOG.md, "
              f"docs/manifest.json) is pushed so the tag points at the right SHA.")
    else:
        rel_assets = " ".join(str(p.relative_to(REPO_ROOT)) for p in release_assets)
        print(f"done.  Upload {asset_names} to the GitHub release.")
        print(f"       firmware.bin is what the on-device OTA checker fetches.")
        print(f"       Paperloom-merged.bin is what the docs/ web installer fetches.")
        print(f"       Body: `{out_notes.relative_to(REPO_ROOT)}`.")
        print(f"       e.g.  gh release create v{args.version} {rel_assets} "
              f"-F {out_notes.relative_to(REPO_ROOT)} -t \"v{args.version}\"")
        print(f"       Or rerun with --publish to do it automatically.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

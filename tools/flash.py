#!/usr/bin/env python3
"""Build, flash, and monitor the firmware on the LilyGo T5S3-4.7-e-paper-PRO.

Usage:
    python tools/flash.py

Optional flags:
    --no-monitor                          skip `pio device monitor` at the end
    --port /dev/cu.usbmodemXXX            override auto-detected serial port
    --skip-build                          flash existing binary, do not rebuild
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ENV = "default"


def find_pio():
    """Locate a working pio binary.

    Homebrew's pio currently bundles Python 3.14, which esptool refuses
    (only 3.10-3.13 supported). PlatformIO Core's managed Python at
    ~/.platformio/penv/bin/python is typically pinned to 3.11, and its
    pio binary there works correctly. Prefer that one when available.
    """
    penv_pio = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    if penv_pio.is_file() and os.access(penv_pio, os.X_OK):
        return str(penv_pio)
    fallback = shutil.which("pio")
    if fallback:
        return fallback
    print("ERROR: pio not found in ~/.platformio/penv/bin or PATH", file=sys.stderr)
    sys.exit(1)


PIO = find_pio()


def run(cmd, cwd=None):
    """Run a command, stream stdout/stderr, return exit code."""
    print(f"\n$ {' '.join(cmd)}", flush=True)
    return subprocess.call(cmd, cwd=cwd)


def main():
    parser = argparse.ArgumentParser(
        description="Build + upload + monitor the Paperloom firmware.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--port", default=None,
                        help="serial port (default: PlatformIO auto-detect)")
    parser.add_argument("--no-monitor", action="store_true",
                        help="skip serial monitor after upload")
    parser.add_argument("--skip-build", action="store_true",
                        help="upload existing build artifacts, do not rebuild")
    args = parser.parse_args()

    project_root = Path(__file__).resolve().parent.parent
    if not (project_root / "platformio.ini").exists():
        print(f"ERROR: platformio.ini not found in {project_root}", file=sys.stderr)
        return 1

    print(f"=== Target: {ENV} ===")
    print(f"=== Project: {project_root} ===")

    port_args = ["--upload-port", args.port] if args.port else []

    if not args.skip_build:
        rc = run([PIO, "run", "-e", ENV], cwd=project_root)
        if rc != 0:
            print(f"\nERROR: build failed (exit {rc})", file=sys.stderr)
            return rc

    rc = run([PIO, "run", "-e", ENV, "-t", "upload"] + port_args, cwd=project_root)
    if rc != 0:
        print(f"\nERROR: upload failed (exit {rc})", file=sys.stderr)
        return rc

    if args.no_monitor:
        return 0

    monitor_cmd = [PIO, "device", "monitor", "-e", ENV]
    if args.port:
        monitor_cmd += ["--port", args.port]
    print(f"\n$ {' '.join(monitor_cmd)}", flush=True)
    os.execvp(monitor_cmd[0], monitor_cmd)


if __name__ == "__main__":
    sys.exit(main())

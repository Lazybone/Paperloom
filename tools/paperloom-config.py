#!/usr/bin/env python3
"""paperloom-config — Configure WiFi / KoSync credentials on a Paperloom
device over USB-Serial without touching the on-device keyboard or Web-UI.

Protocol: TAB-separated args, newline-terminated. Responses are prefixed
OK / OK_BEGIN...OK_END / ERR. Firmware log noise is silently ignored.

Usage:
    paperloom-config.py status    [--port PORT]
    paperloom-config.py scan      [--port PORT]
    paperloom-config.py wifi      <ssid> <password>  [--port PORT]
    paperloom-config.py kosync    <server> <user> <password>
                                  [--device DEVICE] [--port PORT]
    paperloom-config.py reboot    [--port PORT]
    paperloom-config.py raw       "<cmd-string>"  [--port PORT]
"""

import argparse
import sys
import time
from typing import List, Optional, Tuple

try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:
    print("ERROR: pyserial is required. Install with: pip install pyserial", file=sys.stderr)
    sys.exit(1)

BAUD = 115200
DRAIN_SECS = 0.5      # seconds to drain firmware log noise after connect
TIMEOUT_SECS = 10.0   # seconds to wait for a complete response
ESPRESSIF_VID = 0x303A  # Espressif Systems USB VID (S3 built-in CDC)

# Set to True via --debug to print every raw line received from the device.
# Useful to diagnose protocol mismatches (firmware not flashed, wrong port).
DEBUG = False


# ── Port auto-detection ───────────────────────────────────────────────────────

def _find_port() -> Optional[str]:
    """Return the best-guess USB-CDC port for a Paperloom device.

    Priority:
      1. Any port whose USB Vendor ID is 0x303A (Espressif built-in CDC).
      2. Any port whose name contains 'usbmodem' (macOS) or 'usbserial' or
         whose description suggests a CDC device.
    Prints a warning and returns None when multiple candidates are found
    (user should pass --port explicitly).
    """
    candidates = []
    for p in list_ports.comports():
        vid = getattr(p, "vid", None)
        if vid == ESPRESSIF_VID:
            candidates.insert(0, p.device)  # prefer Espressif VID
            continue
        name = p.device.lower()
        desc = (p.description or "").lower()
        if "usbmodem" in name or "usbserial" in name or "com" in name:
            candidates.append(p.device)

    if len(candidates) == 0:
        return None
    if len(candidates) == 1:
        return candidates[0]

    print("Multiple USB serial ports found — pass --port to choose one:")
    for c in candidates:
        print(f"  {c}")
    return None


# ── Serial I/O ────────────────────────────────────────────────────────────────

def _open(port: Optional[str]) -> serial.Serial:
    if port is None:
        port = _find_port()
    if port is None:
        print("ERROR: no USB-CDC port found. Connect the device and try again,\n"
              "       or pass --port /dev/cu.usbmodemXXXX", file=sys.stderr)
        sys.exit(1)
    # IMPORTANT: ESP32 boards auto-reset when DTR/RTS toggle on port open.
    # We use the construct-without-open / set-flags / open pattern so the
    # chip stays running. Without this, every CLI invocation reboots the
    # device, the firmware misses our command, and we time out.
    try:
        ser = serial.Serial()
        ser.port     = port
        ser.baudrate = BAUD
        ser.timeout  = 0.1
        ser.dtr      = False
        ser.rts      = False
        ser.open()
    except serial.SerialException as e:
        print(f"ERROR: cannot open {port}: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"Connected to {port} at {BAUD} baud.")
    # Drain any pending firmware log output.
    deadline = time.monotonic() + DRAIN_SECS
    while time.monotonic() < deadline:
        ser.read(1024)
    return ser



def _send_command(ser: serial.Serial, cmd: str) -> Tuple[bool, List[str]]:
    """Send a command and collect the response.

    Returns (success: bool, lines: List[str]).
    success=True means response started with OK.
    """
    ser.write((cmd + "\n").encode())
    ser.flush()

    lines: List[str] = []
    deadline = time.monotonic() + TIMEOUT_SECS
    multi = False

    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        try:
            line = raw.decode(errors="replace").rstrip("\r\n")
        except Exception:
            continue

        if not line:
            continue

        if DEBUG:
            print(f"  [recv] {line!r}", file=sys.stderr)

        # Single-line OK/ERR response
        if not multi:
            if line.startswith("OK_BEGIN"):
                multi = True
                continue
            if line.startswith("OK"):
                return True, [line]
            if line.startswith("ERR"):
                return False, [line]
            # Firmware noise — ignore
            continue

        # Inside OK_BEGIN block
        if line == "OK_END":
            return True, lines
        # Inside an OK_BEGIN block, collect every non-empty line until OK_END.
        # (Firmware log lines mid-block are rare and harmless to include.)
        lines.append(line)

    print("ERROR: timed out waiting for response.", file=sys.stderr)
    return False, []


# ── Subcommand implementations ────────────────────────────────────────────────

def cmd_status(args):
    ser = _open(args.port)
    try:
        try:
            ok, lines = _send_command(ser, "STATUS")
        except serial.SerialException as e:
            print(f"ERROR: serial communication failed: {e}", file=sys.stderr)
            sys.exit(2)
    finally:
        ser.close()
    if not ok:
        print("\n".join(lines))
        sys.exit(1)
    # Pretty-print as key: value table
    print()
    for line in lines:
        parts = line.split("\t", 1)
        if len(parts) == 2:
            print(f"  {parts[0]:<20} {parts[1]}")
        else:
            print(f"  {line}")
    print()


def cmd_scan(args):
    ser = _open(args.port)
    try:
        print("Scanning… (this takes ~3 seconds)")
        try:
            ok, lines = _send_command(ser, "WIFI_SCAN")
        except serial.SerialException as e:
            print(f"ERROR: serial communication failed: {e}", file=sys.stderr)
            sys.exit(2)
    finally:
        ser.close()
    if not ok:
        print("\n".join(lines))
        sys.exit(1)
    print()
    print(f"  {'RSSI':>5}  {'Enc':>3}  SSID")
    print(f"  {'─'*5}  {'─'*3}  {'─'*40}")
    for line in lines:
        parts = line.split("\t")
        if len(parts) == 4 and parts[0] == "NET":
            rssi = parts[1]
            enc = "WPA" if parts[2] == "1" else "open"
            ssid = parts[3]
            print(f"  {rssi:>5}  {enc:>3}  {ssid}")
    print()


def cmd_wifi(args):
    ser = _open(args.port)
    try:
        try:
            cmd = f"WIFI_SET\t{args.ssid}\t{args.password}"
            ok, lines = _send_command(ser, cmd)
        except serial.SerialException as e:
            print(f"ERROR: serial communication failed: {e}", file=sys.stderr)
            sys.exit(2)
    finally:
        ser.close()
    print("\n".join(lines))
    sys.exit(0 if ok else 1)


def cmd_kosync(args):
    ser = _open(args.port)
    try:
        try:
            device = args.device or ""
            cmd = f"KOSYNC_SET\t{args.server}\t{args.user}\t{args.password}\t{device}"
            ok, lines = _send_command(ser, cmd)
        except serial.SerialException as e:
            print(f"ERROR: serial communication failed: {e}", file=sys.stderr)
            sys.exit(2)
    finally:
        ser.close()
    print("\n".join(lines))
    sys.exit(0 if ok else 1)


def cmd_reboot(args):
    ser = _open(args.port)
    try:
        try:
            ok, lines = _send_command(ser, "REBOOT")
        except serial.SerialException as e:
            print(f"ERROR: serial communication failed: {e}", file=sys.stderr)
            sys.exit(2)
    finally:
        ser.close()
    print("\n".join(lines))
    sys.exit(0 if ok else 1)


def cmd_raw(args):
    ser = _open(args.port)
    try:
        try:
            ok, lines = _send_command(ser, args.command)
        except serial.SerialException as e:
            print(f"ERROR: serial communication failed: {e}", file=sys.stderr)
            sys.exit(2)
    finally:
        ser.close()
    print("\n".join(lines))
    sys.exit(0 if ok else 1)


# ── CLI wiring ────────────────────────────────────────────────────────────────

def main():
    # Parent parser holding --port so EVERY subcommand inherits it.
    # This lets the user write `paperloom-config.py scan --port X` or
    # `paperloom-config.py --port X scan` — both work.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port", default=None,
                        help="Serial port (auto-detected when omitted)")
    common.add_argument("--debug", action="store_true",
                        help="Print every raw line received from the device")

    parser = argparse.ArgumentParser(
        description="Configure Paperloom device over USB-Serial.",
        parents=[common])
    sub = parser.add_subparsers(dest="subcmd", required=True)

    sub.add_parser("status", help="Show device status",
                   parents=[common]).set_defaults(func=cmd_status)
    sub.add_parser("scan",   help="Scan for WiFi networks",
                   parents=[common]).set_defaults(func=cmd_scan)

    p_wifi = sub.add_parser("wifi", help="Set WiFi credentials", parents=[common])
    p_wifi.add_argument("ssid")
    p_wifi.add_argument("password")
    p_wifi.set_defaults(func=cmd_wifi)

    p_ks = sub.add_parser("kosync", help="Set KoSync credentials", parents=[common])
    p_ks.add_argument("server",   help="KoSync server URL (https://...)")
    p_ks.add_argument("user",     help="KoSync username")
    p_ks.add_argument("password", help="KoSync plaintext password (hashed on device)")
    p_ks.add_argument("--device", default=None, help="Device name (optional)")
    p_ks.set_defaults(func=cmd_kosync)

    sub.add_parser("reboot", help="Reboot the device",
                   parents=[common]).set_defaults(func=cmd_reboot)

    p_raw = sub.add_parser("raw", help="Send a raw command (debug)", parents=[common])
    p_raw.add_argument("command", help="Command string with TAB separators")
    p_raw.set_defaults(func=cmd_raw)

    args = parser.parse_args()
    global DEBUG
    DEBUG = getattr(args, "debug", False)
    args.func(args)


if __name__ == "__main__":
    main()

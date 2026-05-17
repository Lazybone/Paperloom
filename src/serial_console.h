#pragma once

// Tiny line-based command parser on the USB-Serial port. Lets a host PC
// configure WiFi/KoSync credentials and trigger reboots without going
// through the on-device keyboard or Web-UI.
//
// Threat model: USB physical access = trusted (same as on-device setup).
// Passwords are never echoed back.
//
// Protocol: TAB-separated args, newline-terminated commands. Responses
// prefixed OK / OK_BEGIN...OK_END / ERR. See tools/paperloom-config.py
// for the host-side counterpart.

void serial_console_tick();

#pragma once

// Shared chrome helpers for all full-screen views.
// Implementations live in src/main.cpp (alongside the layout constants
// W/H/HEADER_HEIGHT/FOOTER_HEIGHT they depend on).
//
// Replaces the per-file `extern void drawHeader(...)` declarations that
// were previously copied across ui_settings, ui_update, ui_keyboard,
// ui_wifi_setup, ui_reader, and ui_library — those copies could silently
// drift if the signature changed.

void drawHeader(const char* title, bool showBattery = true);
void drawBottomBar(const char* label);

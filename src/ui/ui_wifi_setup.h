#pragma once

#include <Arduino.h>
#include "state.h"

// Network selection + on-device WiFi setup screen.
// Owns its own state machine: SCANNING → LIST → (keyboard for ssid/pass) → SAVED.
//
// Caller (main.cpp) wires:
//   - draw  : ui_wifi_setup_draw()
//   - touch : ui_wifi_setup_touch()
//   - tick  : ui_wifi_setup_tick()  — call from main loop while in this state
//   - open  : ui_wifi_setup_open()  — called when entering the screen

void     ui_wifi_setup_open();
void     ui_wifi_setup_close();   // cleanup before leaving (stops scanner)

void     ui_wifi_setup_draw();
void     ui_wifi_setup_tick();
AppState ui_wifi_setup_touch(int x, int y);

// Returns true if there is a redraw-worthy change since the last draw
// (e.g. scan progress finished). main.cpp polls this while in WIFI_SETUP.
bool     ui_wifi_setup_dirty();

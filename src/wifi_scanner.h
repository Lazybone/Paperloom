#pragma once

#include <Arduino.h>

// Async WiFi scanner. Wraps WiFi.scanNetworks(true) in a small state machine
// so the UI can poll progress without blocking the redraw loop.

// Note: enum class to avoid collision with Arduino WiFi.h's
// WIFI_SCAN_RUNNING / WIFI_SCAN_FAILED return-code macros.
enum class WifiScanState {
    Idle,
    Running,
    Done,
    Failed
};

struct WifiNetwork {
    String   ssid;
    int32_t  rssi;
    uint8_t  encryption;  // 0 = open, anything else = secured
};

void wifi_scanner_start();          // Begin async scan; resets prior results
void wifi_scanner_tick();           // Call from main loop to advance state
void wifi_scanner_stop();           // Cancel + free results

WifiScanState wifi_scanner_state();
int           wifi_scanner_count();           // Networks found (0 until DONE)
const WifiNetwork& wifi_scanner_get(int idx); // 0..count()-1

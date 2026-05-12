#include "wifi_scanner.h"
#include <WiFi.h>
#include <vector>

static WifiScanState        _state = WifiScanState::Idle;
static std::vector<WifiNetwork> _results;
static unsigned long        _scanStart = 0;
static const unsigned long  SCAN_TIMEOUT_MS = 12000;

void wifi_scanner_start() {
    // Tear down any previous WiFi session (upload/STA) so the scan starts
    // from a clean radio state.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    _results.clear();
    _state = WifiScanState::Running;
    _scanStart = millis();

    // Note: WIFI_SCAN_FAILED here is the Arduino WiFi.h sentinel (-2),
    // not WifiScanState::Failed.
    int rc = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
    if (rc == WIFI_SCAN_FAILED) {
        _state = WifiScanState::Failed;
        Serial.println("WiFi scanner: scanNetworks() returned FAILED");
    } else {
        Serial.println("WiFi scanner: async scan started");
    }
}

void wifi_scanner_tick() {
    if (_state != WifiScanState::Running) return;

    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {  // Arduino sentinel (-1)
        if (millis() - _scanStart >= SCAN_TIMEOUT_MS) {
            Serial.println("WiFi scanner: timeout");
            WiFi.scanDelete();
            _state = WifiScanState::Failed;
        }
        return;
    }
    if (n < 0) {
        _state = WifiScanState::Failed;
        return;
    }

    _results.reserve(n);
    for (int i = 0; i < n; i++) {
        WifiNetwork net;
        net.ssid       = WiFi.SSID(i);
        net.rssi       = WiFi.RSSI(i);
        net.encryption = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? 0 : 1;
        if (net.ssid.length() == 0) continue;  // skip blank/hidden
        _results.push_back(net);
    }
    WiFi.scanDelete();
    _state = WifiScanState::Done;
    Serial.printf("WiFi scanner: %d networks (filtered)\n", (int)_results.size());
}

void wifi_scanner_stop() {
    if (_state == WifiScanState::Running) {
        WiFi.scanDelete();
    }
    _results.clear();
    _state = WifiScanState::Idle;
}

WifiScanState wifi_scanner_state() { return _state; }
int           wifi_scanner_count() { return (int)_results.size(); }

const WifiNetwork& wifi_scanner_get(int idx) {
    static WifiNetwork empty;
    if (idx < 0 || idx >= (int)_results.size()) return empty;
    return _results[idx];
}

#include "ui_wifi_setup.h"
#include "ui_keyboard.h"
#include "config.h"
#include "../display.h"
#include "../wifi_scanner.h"
#include "../settings.h"

#include "ui_common.h"

// ─── Layout ────────────────────────────────────────────────────────
static const int W = PORTRAIT_W;
static const int H = PORTRAIT_H;

static const int STATUS_Y    = HEADER_HEIGHT + 16;
static const int LIST_TOP    = HEADER_HEIGHT + 76;
static const int ROW_H       = 72;
static const int FOOTER_TOP  = H - FOOTER_HEIGHT;
static const int LIST_BOTTOM = FOOTER_TOP - 10;
static const int VISIBLE_ROWS = (LIST_BOTTOM - LIST_TOP) / ROW_H;

// Footer button widths
static const int FN_BACK_W    = 120;
static const int FN_REFRESH_W = 140;
static const int FN_PAGE_W    = (W - FN_BACK_W - FN_REFRESH_W) / 2;

// ─── Module state ──────────────────────────────────────────────────
static int    _scrollOffset   = 0;
static bool   _dirty          = true;
static String _selectedSSID;     // SSID picked from list or entered manually
static bool   _selectedSecured = false;

// ─── Helpers ───────────────────────────────────────────────────────

// Map RSSI to a 0..4 signal-strength bucket.
static int rssi_bars(int32_t rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

static int total_rows() {
    // First row is "Manual Entry"; remaining are scan results.
    return 1 + (wifi_scanner_state() == WifiScanState::Done ? wifi_scanner_count() : 0);
}

static void clamp_scroll() {
    int total = total_rows();
    int maxScroll = total - VISIBLE_ROWS;
    if (maxScroll < 0) maxScroll = 0;
    if (_scrollOffset > maxScroll) _scrollOffset = maxScroll;
    if (_scrollOffset < 0) _scrollOffset = 0;
}

// ─── Keyboard callbacks ────────────────────────────────────────────

static AppState keyboard_cancel_back_to_setup() {
    _dirty = true;
    return STATE_WIFI_SETUP;
}

static AppState keyboard_password_done(const String& pass) {
    Settings& s = settings_get();
    s.wifiSSID = _selectedSSID;
    s.wifiPass = pass;
    if (!settings_save()) {
        Serial.println("WiFi setup: settings_save failed (SD?)");
        // The in-memory settings still hold the new credentials, so the
        // user can attempt a connection — but the value won't survive
        // a reboot. Surface in the next draw via _dirty.
    }
    Serial.printf("WiFi setup: saved SSID=%s\n", s.wifiSSID.c_str());
    _dirty = true;
    return STATE_WIFI_SETUP;
}

static AppState keyboard_open_password();  // forward

static AppState keyboard_manual_ssid_done(const String& ssid) {
    if (ssid.length() == 0) {
        _dirty = true;
        return STATE_WIFI_SETUP;
    }
    _selectedSSID   = ssid;
    _selectedSecured = true;  // assume secured for manual entries
    return keyboard_open_password();
}

static AppState keyboard_open_password() {
    String prompt = String("Password for ") + _selectedSSID;
    ui_keyboard_open(prompt.c_str(), String(""), /*isPassword=*/true,
                     keyboard_password_done,
                     keyboard_cancel_back_to_setup);
    return STATE_WIFI_KEYBOARD;
}

static AppState keyboard_open_manual_ssid() {
    ui_keyboard_open("Enter network SSID", String(""), /*isPassword=*/false,
                     keyboard_manual_ssid_done,
                     keyboard_cancel_back_to_setup);
    return STATE_WIFI_KEYBOARD;
}

// ─── Public API ────────────────────────────────────────────────────

void ui_wifi_setup_open() {
    _scrollOffset = 0;
    _selectedSSID = "";
    _selectedSecured = false;
    _dirty = true;
    wifi_scanner_start();
}

void ui_wifi_setup_close() {
    wifi_scanner_stop();
}

void ui_wifi_setup_tick() {
    WifiScanState before = wifi_scanner_state();
    wifi_scanner_tick();
    WifiScanState after = wifi_scanner_state();
    if (before != after) _dirty = true;
}

bool ui_wifi_setup_dirty() {
    bool d = _dirty;
    _dirty = false;
    return d;
}

// ─── Drawing ───────────────────────────────────────────────────────

static void draw_signal_bars(int x, int y, int bars) {
    // Four small ascending bars; filled = active.
    const int barW = 6;
    const int gap  = 3;
    const int baseH = 8;
    for (int i = 0; i < 4; i++) {
        int bx = x + i * (barW + gap);
        int bh = baseH + i * 6;
        int by = y - bh;
        if (i < bars) {
            display_draw_filled_rect(bx, by, barW, bh, 0);
        } else {
            display_draw_rect(bx, by, barW, bh, 0);
        }
    }
}

static void draw_status() {
    Settings& s = settings_get();
    char line[96];
    if (s.wifiSSID.length() > 0) {
        snprintf(line, sizeof(line), "Current: %s", s.wifiSSID.c_str());
    } else {
        snprintf(line, sizeof(line), "No network configured");
    }
    display_draw_text(MARGIN_X, STATUS_Y + display_font_height(), line, 0);

    // Build the right-aligned status string into a stack buffer instead of
    // pointing into a function-local static — the static was vulnerable to
    // stale-content races if draw_status() was called twice between mutex-
    // free state transitions of the scanner.
    char sub[40];
    sub[0] = '\0';
    switch (wifi_scanner_state()) {
        case WifiScanState::Running:
            snprintf(sub, sizeof(sub), "Scanning…");
            break;
        case WifiScanState::Failed:
            snprintf(sub, sizeof(sub), "Scan failed — tap Refresh");
            break;
        case WifiScanState::Done:
            snprintf(sub, sizeof(sub), "%d networks found", wifi_scanner_count());
            break;
        default:
            break;
    }
    if (sub[0] != '\0') {
        int sw = display_text_width(sub);
        display_draw_text(W - MARGIN_X - sw, STATUS_Y + display_font_height(), sub, 6);
    }
}

static void draw_row(int rowY, int idxInList) {
    // First entry in the list is always "Manual Entry"
    if (idxInList == 0) {
        display_draw_text(MARGIN_X, rowY + ROW_H / 2 + 8, "[ Manual Entry ]", 0);
        return;
    }

    int scanIdx = idxInList - 1;
    if (scanIdx < 0 || scanIdx >= wifi_scanner_count()) return;
    const WifiNetwork& net = wifi_scanner_get(scanIdx);

    // SSID (left), truncated to fit
    String ssid = net.ssid;
    int maxSsidPx = W - MARGIN_X * 2 - 160;
    while (ssid.length() > 0 && display_text_width(ssid.c_str()) > maxSsidPx) {
        ssid = ssid.substring(0, ssid.length() - 1);
    }
    display_draw_text(MARGIN_X, rowY + ROW_H / 2 + 8, ssid.c_str(), 0);

    // Lock indicator (right side)
    int rightX = W - MARGIN_X;
    if (net.encryption != 0) {
        const char* lock = "[L]";
        int lw = display_text_width(lock);
        rightX -= lw;
        display_draw_text(rightX, rowY + ROW_H / 2 + 8, lock, 0);
        rightX -= 12;
    }

    // Signal bars
    draw_signal_bars(rightX - 36, rowY + ROW_H / 2 + 12, rssi_bars(net.rssi));
}

void ui_wifi_setup_draw() {
    display_set_font_size(2);  // chrome always in Inter
    display_fill_screen(15);
    drawHeader("WiFi Setup", true);

    draw_status();

    // List rows
    int total = total_rows();
    clamp_scroll();
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        int idx = _scrollOffset + i;
        if (idx >= total) break;
        int rowY = LIST_TOP + i * ROW_H;
        draw_row(rowY, idx);
        // Separator
        display_draw_hline(MARGIN_X, rowY + ROW_H - 2, W - MARGIN_X * 2, 10);
    }

    // Dark footer bar matching the app-wide style: grey-2 background,
    // white labels, thin separators between segments.
    bool canScrollUp = _scrollOffset > 0;
    bool canScrollDn = _scrollOffset + VISIBLE_ROWS < total;
    display_draw_filled_rect(0, FOOTER_TOP, W, FOOTER_HEIGHT, 2);

    auto drawSeg = [&](int x, int w, const char* label, bool dim) {
        int tw = display_text_width(label);
        display_draw_text(x + (w - tw) / 2,
                          FOOTER_TOP + FOOTER_HEIGHT - 12,
                          label, dim ? 9 : 15);
    };

    int fx = 0;
    drawSeg(fx, FN_BACK_W, "Back", false);
    fx += FN_BACK_W;
    display_draw_filled_rect(fx - 1, FOOTER_TOP + 4, 2, FOOTER_HEIGHT - 8, 10);
    drawSeg(fx, FN_REFRESH_W, "Refresh", false);
    fx += FN_REFRESH_W;
    display_draw_filled_rect(fx - 1, FOOTER_TOP + 4, 2, FOOTER_HEIGHT - 8, 10);
    drawSeg(fx, FN_PAGE_W, "< Prev", !canScrollUp);
    fx += FN_PAGE_W;
    display_draw_filled_rect(fx - 1, FOOTER_TOP + 4, 2, FOOTER_HEIGHT - 8, 10);
    drawSeg(fx, FN_PAGE_W, "Next >", !canScrollDn);

    display_update_fast();
}

// ─── Touch handling ────────────────────────────────────────────────

AppState ui_wifi_setup_touch(int x, int y) {
    // Footer
    if (y >= FOOTER_TOP) {
        if (x < FN_BACK_W) {
            ui_wifi_setup_close();
            return STATE_SETTINGS;
        }
        if (x < FN_BACK_W + FN_REFRESH_W) {
            wifi_scanner_start();
            _scrollOffset = 0;
            _dirty = true;
            return STATE_WIFI_SETUP;
        }
        int prevX = FN_BACK_W + FN_REFRESH_W;
        if (x < prevX + FN_PAGE_W) {
            _scrollOffset -= VISIBLE_ROWS;
            clamp_scroll();
            _dirty = true;
            return STATE_WIFI_SETUP;
        }
        // Next page
        _scrollOffset += VISIBLE_ROWS;
        clamp_scroll();
        _dirty = true;
        return STATE_WIFI_SETUP;
    }

    // List row hit
    if (y >= LIST_TOP && y < LIST_TOP + VISIBLE_ROWS * ROW_H) {
        int rowInPage = (y - LIST_TOP) / ROW_H;
        int idx = _scrollOffset + rowInPage;
        int total = total_rows();
        if (idx < 0 || idx >= total) return STATE_WIFI_SETUP;

        if (idx == 0) {
            // Manual Entry
            return keyboard_open_manual_ssid();
        }

        int scanIdx = idx - 1;
        if (scanIdx < 0 || scanIdx >= wifi_scanner_count()) return STATE_WIFI_SETUP;
        const WifiNetwork& net = wifi_scanner_get(scanIdx);
        _selectedSSID    = net.ssid;
        _selectedSecured = (net.encryption != 0);

        if (!_selectedSecured) {
            // Open network — save with empty password.
            Settings& s = settings_get();
            s.wifiSSID = _selectedSSID;
            s.wifiPass = "";
            if (!settings_save()) {
                Serial.println("WiFi setup: save failed (open network) — credential lost on reboot");
            }
            Serial.printf("WiFi setup: saved open network %s\n", s.wifiSSID.c_str());
            _dirty = true;
            return STATE_WIFI_SETUP;
        }
        return keyboard_open_password();
    }

    return STATE_WIFI_SETUP;
}

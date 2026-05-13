#include "ui_update.h"
#include "config.h"
#include "../settings.h"
#include "../display.h"
#include "../wifi_upload.h"
#include "../library.h"
#include "../cover_renderer.h"
#include "ota_update.h"
#include <WiFi.h>
#include <qrcode.h>
#include <functional>
#include <cstring>

// ─── Extern declarations for shared helpers in main.cpp ─────────────
#include "ui_common.h"

// ─── Layout constants ───────────────────────────────────────────────
static const int W = PORTRAIT_W;
static const int H = PORTRAIT_H;
static const int FONT_H = 50;

// ═══════════════════════════════════════════════════════════════════
// OTA Update screen
// ═══════════════════════════════════════════════════════════════════

void ui_ota_draw(OtaState& otaState) {
    display_set_font_size(2);  // chrome always in Inter
    display_fill_screen(15);
    drawHeader("Firmware Update");

    int cy = H / 2 - 40;

    switch (otaState.phase) {
        case OTA_CHECKING: {
            const char* msg = "Checking for updates...";
            int mw = display_text_width(msg);
            display_draw_text((W - mw) / 2, cy, msg, 0);
            drawBottomBar("[ Cancel ]");
            display_begin_frame();
            display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
            display_flush();
            break;
        }

        case OTA_RESULT: {
            char msg[64];
            // ota_check_for_update populates latestVersion only on a
            // successful API response. An empty string means the fetch
            // itself failed (WiFi, TLS, HTTP error, parse error) — don't
            // claim "up to date" in that case, the device may simply not
            // have seen the latest release yet.
            const bool fetchOk = otaState.latestVersion.length() > 0
                && (otaState.latestVersion[0] == 'v' || otaState.latestVersion[0] == 'V'
                    || (otaState.latestVersion[0] >= '0' && otaState.latestVersion[0] <= '9'));
            if (otaState.updateAvailable) {
                snprintf(msg, sizeof(msg), "%s available", otaState.latestVersion.c_str());
                int mw = display_text_width(msg);
                display_draw_text((W - mw) / 2, cy, msg, 0);

                const char* tap = "Tap to install";
                int tw = display_text_width(tap);
                display_draw_text((W - tw) / 2, cy + FONT_H + 16, tap, 3);
            } else if (fetchOk) {
                snprintf(msg, sizeof(msg), "Up to date (v%s)", FIRMWARE_VERSION);
                int mw = display_text_width(msg);
                display_draw_text((W - mw) / 2, cy, msg, 0);
            } else {
                const char* msg1 = "Update check failed";
                int mw = display_text_width(msg1);
                display_draw_text((W - mw) / 2, cy, msg1, 0);
                char detail[80];
                snprintf(detail, sizeof(detail), "Running v%s — try again", FIRMWARE_VERSION);
                int dw = display_text_width(detail);
                display_draw_text((W - dw) / 2, cy + FONT_H + 16, detail, 3);
            }
            drawBottomBar("[ Back ]");
            display_begin_frame();
            display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
            display_flush();
            break;
        }

        case OTA_DOWNLOADING: {
            const char* msg = "Downloading update...";
            int mw = display_text_width(msg);
            display_draw_text((W - mw) / 2, cy - 20, msg, 0);

            int barX = MARGIN_X + 20;
            int barW = W - barX * 2;
            int barY = cy + 30;
            int barH = 16;
            display_draw_rect(barX, barY, barW, barH, 0);
            int fillW = (barW - 4) * otaState.progress / 100;
            if (fillW > 0) {
                display_draw_filled_rect(barX + 2, barY + 2, fillW, barH - 4, 0);
            }

            char pctStr[16];
            snprintf(pctStr, sizeof(pctStr), "%d%%", otaState.progress);
            int pw = display_text_width(pctStr);
            display_draw_text((W - pw) / 2, barY + barH + 20, pctStr, 0);

            display_begin_frame();
            display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
            display_flush();
            break;
        }

        case OTA_DONE: {
            const char* msg = "Update complete!";
            int mw = display_text_width(msg);
            display_draw_text((W - mw) / 2, cy, msg, 0);

            const char* msg2 = "Restarting...";
            int m2w = display_text_width(msg2);
            display_draw_text((W - m2w) / 2, cy + FONT_H + 16, msg2, 3);

            // Final screen before reboot — request an explicit clean GC16
            // refresh so the "Update complete!" message is rendered without
            // any partial-update artifacts. Also resets the anti-ghost
            // counter, which is harmless given the imminent restart.
            display_force_full_refresh();

            delay(2000);
            esp_restart();
            break;
        }

        case OTA_FAILED: {
            const char* msg = "Update failed";
            int mw = display_text_width(msg);
            display_draw_text((W - mw) / 2, cy, msg, 0);

            if (otaState.latestVersion.length() > 0) {
                int lw = display_text_width(otaState.latestVersion.c_str());
                display_draw_text((W - lw) / 2, cy + FONT_H + 16, otaState.latestVersion.c_str(), 6);
            }

            drawBottomBar("[ Back ]");
            display_begin_frame();
            display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
            display_flush();
            break;
        }

        default:
            break;
    }
}

void ui_ota_tick(OtaState& otaState) {
    if (otaState.phase != OTA_CHECKING) return;

    if (WiFi.status() != WL_CONNECTED) {
        Settings& s = settings_get();
        WiFi.mode(WIFI_STA);
        WiFi.begin(s.wifiSSID.c_str(), s.wifiPass.c_str());
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(250);
        }
    }

    if (WiFi.status() != WL_CONNECTED) {
        otaState.phase = OTA_FAILED;
        otaState.latestVersion = "WiFi connect failed";
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return;
    }
    otaState.updateAvailable = ota_check_for_update(otaState.latestVersion);
    otaState.phase = OTA_RESULT;
}

AppState ui_ota_touch(int x, int y, OtaState& otaState) {
    (void)x;

    // Footer → back (cancel)
    if (y > H - FOOTER_HEIGHT) {
        otaState.phase = OTA_IDLE;
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return STATE_SETTINGS;
    }

    // In result state with update available → start download
    if (otaState.phase == OTA_RESULT && otaState.updateAvailable) {
        otaState.phase = OTA_DOWNLOADING;
        otaState.progress = 0;

        // Draw the initial download screen
        ui_ota_draw(otaState);

        // Perform the download (blocking with progress callbacks)
        bool ok = ota_install_update([&otaState](int pct) {
            otaState.progress = pct;
            if (pct % 5 == 0) {
                display_fill_screen(15);
                drawHeader("Firmware Update");

                int cy = H / 2 - 40;
                const char* msg = "Downloading update...";
                int mw = display_text_width(msg);
                display_draw_text((W - mw) / 2, cy - 20, msg, 0);

                int barX = MARGIN_X + 20;
                int barW = W - barX * 2;
                int barY = cy + 30;
                int barH = 16;
                display_draw_rect(barX, barY, barW, barH, 0);
                int fillW = (barW - 4) * pct / 100;
                if (fillW > 0) {
                    display_draw_filled_rect(barX + 2, barY + 2, fillW, barH - 4, 0);
                }

                char pctStr[16];
                snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
                int pw = display_text_width(pctStr);
                display_draw_text((W - pw) / 2, barY + barH + 20, pctStr, 0);

                // Progress callback redraws the entire screen (fill +
                // header + bar + percentage), not just the progress bar
                // region — so this is a StructuralRedraw, not a small
                // GlyphTick. The anti-ghost counter will auto-promote to
                // a clean GC16 every REFRESH_INTERVAL_READER ticks.
                display_begin_frame();
                display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
                display_flush();
            }
        });

        if (ok) {
            otaState.phase = OTA_DONE;
        } else {
            otaState.phase = OTA_FAILED;
            otaState.latestVersion = "Try again later";
        }
        return STATE_OTA_CHECK;
    }

    // In failed state → back
    if (otaState.phase == OTA_FAILED || (otaState.phase == OTA_RESULT && !otaState.updateAvailable)) {
        otaState.phase = OTA_IDLE;
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return STATE_SETTINGS;
    }

    return STATE_OTA_CHECK;
}

// ═══════════════════════════════════════════════════════════════════
// WiFi Manager screen
// ═══════════════════════════════════════════════════════════════════

static void drawQrCode(const char* text, int cx, int cy, int moduleSize) {
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcodeData, 3, ECC_LOW, text);

    int qrSize = qrcode.size * moduleSize;
    int startX = cx - qrSize / 2;
    int startY = cy - qrSize / 2;

    display_draw_filled_rect(startX - 8, startY - 8, qrSize + 16, qrSize + 16, 15);

    for (uint8_t row = 0; row < qrcode.size; row++) {
        for (uint8_t col = 0; col < qrcode.size; col++) {
            if (qrcode_getModule(&qrcode, col, row)) {
                display_draw_filled_rect(startX + col * moduleSize,
                                          startY + row * moduleSize,
                                          moduleSize, moduleSize, 0);
            }
        }
    }
}

// Animation state for connecting dots
static int connectDots = 0;
static unsigned long lastDotsUpdate = 0;

void ui_wifi_draw() {
    display_set_font_size(2);  // chrome always in Inter
    display_fill_screen(15);
    drawHeader("WiFi Manager");

    int y = HEADER_HEIGHT + 40;

    if (wifi_upload_running()) {
        const char* l1 = "WiFi connected";
        int w1 = display_text_width(l1);
        display_draw_text((W - w1) / 2, y, l1, 0);
        y += FONT_H + 12;

        String ipStr = "http://" + wifi_upload_ip();
        int w2 = display_text_width(ipStr.c_str());
        display_draw_text((W - w2) / 2, y, ipStr.c_str(), 0);
        y += FONT_H + 12;

        const char* l3 = "Scan to open upload page";
        int w3 = display_text_width(l3);
        display_draw_text((W - w3) / 2, y, l3, 0);
        y += FONT_H + 20;

        drawQrCode(ipStr.c_str(), W / 2, y + 120, 5);
    } else if (wifi_upload_has_error()) {
        const char* l1 = "WiFi Error";
        int w1 = display_text_width(l1);
        display_draw_text((W - w1) / 2, y, l1, 0);
        y += FONT_H + 16;

        const char* errorMsg = wifi_upload_get_error();
        if (strlen(errorMsg) > 0) {
            int w2 = display_text_width(errorMsg);
            display_draw_text((W - w2) / 2, y, errorMsg, 6);
        }
        y += FONT_H + 30;

        const char* hint = "Check WiFi settings";
        int hw = display_text_width(hint);
        display_draw_text((W - hw) / 2, y, hint, 8);
    } else if (wifi_upload_connecting()) {
        const char* baseText = "Connecting to WiFi";
        int w1 = display_text_width(baseText);
        display_draw_text((W - w1) / 2, y, baseText, 0);

        if (millis() - lastDotsUpdate > 500) {
            connectDots = (connectDots + 1) % 4;
            lastDotsUpdate = millis();
        }
        char dots[5] = {};
        int n = (connectDots > 4) ? 4 : connectDots;
        snprintf(dots, sizeof(dots), "%.*s", n, "....");
        display_draw_text((W + w1) / 2 + 8, y, dots, 0);

        y += FONT_H + 30;

        int barX = MARGIN_X + 40;
        int barW = W - barX * 2;
        int barY = y;
        int barH = 12;
        display_draw_rect(barX, barY, barW, barH, 0);

        unsigned long elapsed = millis() % 2000;
        int fillW = (barW - 4) * elapsed / 2000;
        if (fillW > 0) {
            display_draw_filled_rect(barX + 2, barY + 2, fillW, barH - 4, 0);
        }
    } else {
        const char* l1 = "Starting WiFi...";
        int w1 = display_text_width(l1);
        display_draw_text((W - w1) / 2, y, l1, 4);
    }

    // Standard dark footer bar (matches every other full-screen view in the app).
    drawBottomBar("[ Back ]");

    display_begin_frame();
    display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
    display_flush();
}

AppState ui_wifi_touch(int x, int y,
                       std::vector<BookInfo>& books,
                       std::vector<int>& filteredIndices) {
    (void)x; (void)y;
    wifi_upload_stop();
    books = library_scan();
    // filteredIndices will be updated by caller with current filter
    cover_cache_clear();
    return STATE_LIBRARY;
}

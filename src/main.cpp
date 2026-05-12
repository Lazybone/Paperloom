#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "settings.h"
#include "display.h"
#include "touch.h"
#include "battery.h"
#include "hw_disable_unused.h"
#include "library.h"
#include "reader.h"
#include "cover_renderer.h"
#include "sleep_image.h"
#include "wifi_upload.h"
#include "inline_image.h"
#include "ota_update.h"
#include "splash_screen.h"
#include "frontlight.h"
#include "button_action.h"
#include "state.h"
#include "debug_trace.h"
#include "ui/ui_library.h"
#include "ui/ui_reader.h"
#include "ui/ui_settings.h"
#include "ui/ui_update.h"
#include "ui/ui_keyboard.h"
#include "ui/ui_wifi_setup.h"
#include <WiFi.h>
#include <Preferences.h>
#include <qrcode.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <driver/gpio.h>

// ─── App State ──────────────────────────────────────────────────────
static AppState       appState = STATE_BOOT;
static bool           firstLibraryDraw = true;  // full refresh after splash to clear ghost
static bool           settingsSoftRefreshOnce = false;  // lighter one-shot refresh when entering Settings from another screen
static unsigned long  lastActivity = 0;
static bool           needsRedraw = true;
static unsigned long  bootTime = 0;          // millis() at setup() end
// Light sleep stays opt-out (false) until the wake-from-touch-INT path is
// rewired — earlier attempts re-armed GPIO 47 and caused immediate wake
// loops. The infrastructure (configureLightSleep/canEnterLightSleep) is
// kept in place so a future fix only needs to flip this flag and verify.
static bool           lightSleepEnabled = false;
static unsigned long  lastTouchOrButtonTime = 0;  // last physical input
static ReaderRefreshState readerRefresh = {false, false, false, 0};

// ─── Library state ──────────────────────────────────────────────────
static std::vector<BookInfo> books;
static int libraryScroll = 0;
static LibraryFilter libraryFilter = FILTER_ALL;
static std::vector<int> filteredIndices;  // indices into books[] matching current filter

// ─── Reader state ───────────────────────────────────────────────────
BookReader reader;

// ─── UI module callbacks ─────────────────────────────────────────────
void setAppState(AppState state) { appState = state; }
void setNeedsRedraw(bool val) { needsRedraw = val; }
void setReaderFastRefresh(bool val) { readerRefresh.fastRefresh = val; }
void resetReaderPageTurns() { readerRefresh.pageTurnsSinceFull = 0; }

// ─── TOC / Bookmarks scroll ─────────────────────────────────────────
static int tocScroll = 0;
static int bmScroll = 0;

// ─── Long-press tracking ────────────────────────────────────────────
static unsigned long touchDownTime = 0;
static bool touchHandled = false;
static const unsigned long LONG_PRESS_MS = 800;

// ─── Bedien-Button: konfigurierbar über Settings (bootButton*) ────
// BOOT-Button auf GPIO 0 (BUTTON_PIN aus config.h). Tap/Double/Long
// werden via button_action_execute() dispatched. Long-Press fällt auf
// BTN_ACTION_SLEEP zurück, wenn bootButtonEnabled=false (Fail-Safe).
// Die PCA9535-basierten User-Tasten werden separat via PCA9535-IO12
// abgefragt (siehe USER_BUTTON_VIA_PCA9535 weiter unten).
static unsigned long btnDownTime = 0;
static bool btnWasPressed = false;
static unsigned long lastBtnReleaseTime = 0;
static int btnPressCount = 0;
static bool btnLongFired = false;  // suppress tap-count after long fires
static const unsigned long BUTTON_DEBOUNCE_MS = 50;
static const unsigned long BUTTON_POWER_MS = 600;   // hold 600ms to sleep
// 250 ms is enough to register an intentional double-tap on a physical
// button without the user perceiving a single-press as laggy. The previous
// 400 ms made every page-turn feel sluggish even though the e-paper refresh
// itself is the bigger contributor.
static const unsigned long DOUBLE_PRESS_WINDOW_MS = 250;

// User button (USER_BUTTON_PIN) — independent gesture state.
static unsigned long ubtnDownTime = 0;
static bool          ubtnWasPressed = false;
static unsigned long ubtnLastReleaseTime = 0;
static int           ubtnPressCount = 0;
static bool          ubtnLongFired = false;  // suppress tap-count after long fires
// If the pin reads LOW at boot and never releases, treat it as
// not-connected (no physical button on this hardware variant) and disable
// gesture handling so we don't fire actions on a stuck input.
static bool          ubtnPresent = false;
static bool          ubtnInitDone = false;
static unsigned long ubtnInitStart = 0;
static unsigned long wakeCooldownEnd = 0;  // No-sleep period after wake

// ─── OTA state ──────────────────────────────────────────────────────
static OtaState otaState = {OTA_IDLE, "", false, 0};

// ─── Layout helpers ─────────────────────────────────────────────────
static const int W = PORTRAIT_W;
static const int H = PORTRAIT_H;
static const int FONT_H = 50;  // UI font height — medium reader font advance_y

// Book list item height
static const int BOOK_ITEM_H = FONT_H * 2 + 12;  // title + info + padding


static void showWakeFeedback() {
    const int bannerH = 110;
    const int y = H - bannerH;

    // Wake banner overlays the sleep image after the user presses a key.
    // Chrome text always renders in the UI font (Inter), regardless of
    // whichever reader font was last selected before sleep.
    display_set_font_size(2);

    display_draw_filled_rect(0, y, W, bannerH, 15);
    display_draw_hline(0, y, W, 10);

    const char* line1 = "Waking...";
    int w1 = display_text_width(line1);
    display_draw_text((W - w1) / 2, y + 42, line1, 2);

    display_draw_filled_rect(80, y + 76, W - 160, 8, 12);
    display_draw_filled_rect(80, y + 76, (W - 160) / 3, 8, 4);

    // Update only the banner so the sleep image remains visible elsewhere,
    // giving immediate confirmation that the wake button press was accepted.
    display_update_reader_body(0, y, W, bannerH, false);
}

static void enterDeepSleep(bool triggeredByButton = false);

// ═══════════════════════════════════════════════════════════════════
// Drawing helpers
// ═══════════════════════════════════════════════════════════════════

static const char* display_version_text() {
    static char verBuf[32];
    if (FIRMWARE_VERSION[0] == 'v' || FIRMWARE_VERSION[0] == 'V') {
        snprintf(verBuf, sizeof(verBuf), "%s", FIRMWARE_VERSION);
    } else {
        snprintf(verBuf, sizeof(verBuf), "v%s", FIRMWARE_VERSION);
    }
    return verBuf;
}

void drawHeader(const char* title, bool showBattery = true) {
    display_draw_filled_rect(0, 0, W, HEADER_HEIGHT, 2);
    display_draw_text(MARGIN_X, HEADER_HEIGHT - 18, title, 15);

    if (showBattery && settings_get().showBattery) {
        char battStr[16];
        snprintf(battStr, sizeof(battStr), "%d%%", battery_percent());
        int bw = display_text_width(battStr);
        display_draw_text(W - MARGIN_X - bw, HEADER_HEIGHT - 18, battStr, 15);
    }

    display_draw_hline(0, HEADER_HEIGHT, W, 0);
}

void drawBottomBar(const char* label) {
    int barY = H - FOOTER_HEIGHT;
    display_draw_filled_rect(0, barY, W, FOOTER_HEIGHT, 2);
    display_draw_hline(0, barY, W, 0);
    int tw = display_text_width(label);
    display_draw_text((W - tw) / 2, barY + FOOTER_HEIGHT - 12, label, 15);
}

// Split bottom bar: two buttons
static void drawBottomBarSplit(const char* left, const char* right) {
    int barY = H - FOOTER_HEIGHT;
    display_draw_filled_rect(0, barY, W, FOOTER_HEIGHT, 2);
    display_draw_hline(0, barY, W, 0);
    // Vertical divider
    display_draw_filled_rect(W / 2 - 1, barY + 4, 2, FOOTER_HEIGHT - 8, 10);
    // Left label
    int lw = display_text_width(left);
    display_draw_text(W / 4 - lw / 2, barY + FOOTER_HEIGHT - 12, left, 15);
    // Right label
    int rw = display_text_width(right);
    display_draw_text(W * 3 / 4 - rw / 2, barY + FOOTER_HEIGHT - 12, right, 15);
}

static void drawSplashScreen(const char* statusMsg = "Starting up...") {
    display_fill_screen(15);

    for (int y = 0; y < SPLASH_ART_HEIGHT; ++y) {
        int rowOffset = y * ((SPLASH_WIDTH + 7) / 8);
        for (int x = 0; x < SPLASH_WIDTH; ++x) {
            uint8_t byte = pgm_read_byte(&SPLASH_BITMAP[rowOffset + (x / 8)]);
            bool black = (byte & (0x80 >> (x & 7))) == 0;
            display_draw_pixel(x, y, black ? 0 : 15);
        }
    }

    // Frameless status band over the lower portion of the splash bitmap.
    // White backdrop keeps text readable over the artwork; no border lines.
    const int footerTop = H - 140;
    const int bottomY = H - 8;
    display_draw_filled_rect(0, footerTop, SPLASH_WIDTH, H - footerTop, 15);

    display_set_font_size(1);
    const int splashFontH = display_font_height();
    const int versionY = bottomY - 10;
    const int statusY  = max(footerTop + splashFontH + 10,
                             versionY - splashFontH - 10);

    int statusW = display_text_width(statusMsg);
    display_draw_text((SPLASH_WIDTH - statusW) / 2, statusY, statusMsg, 0);

    const char* verStr = display_version_text();
    int vw = display_text_width(verStr);
    display_draw_text((SPLASH_WIDTH - vw) / 2, versionY, verStr, 8);

    display_set_font_size(2);
}

// ═══════════════════════════════════════════════════════════════════
// Library screen
// ═══════════════════════════════════════════════════════════════════

static const int FILTER_TAB_H = 44;

static void updateFilteredIndices() {
    filteredIndices = library_filter(books, libraryFilter);
}

static void drawLibraryScreen() {
    ui_library_draw(books, libraryScroll, (int)libraryFilter, filteredIndices, firstLibraryDraw);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Reader screen
// ═══════════════════════════════════════════════════════════════════

static void drawReaderScreen() {
    ui_reader_draw(reader, readerRefresh);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Menu overlay (redesigned)
// ═══════════════════════════════════════════════════════════════════

static void drawMenuOverlay() {
    ui_reader_menu_draw(reader);
    needsRedraw = false;
}

static void drawGotoScreen() {
    ui_reader_goto_draw(reader);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// TOC screen
// ═══════════════════════════════════════════════════════════════════

static void drawTocScreen() {
    ui_reader_toc_draw(reader, tocScroll);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Bookmarks screen
// ═══════════════════════════════════════════════════════════════════

static void drawBookmarksScreen() {
    ui_reader_bookmarks_draw(reader, bmScroll);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// Settings screen (delegated to ui/ui_settings)
// ═══════════════════════════════════════════════════════════════════

static void drawSettingsScreen() {
    ui_settings_draw(settingsSoftRefreshOnce);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// OTA Update screen (delegated to ui/ui_update)
// ═══════════════════════════════════════════════════════════════════

static void drawOtaScreen() {
    ui_ota_draw(otaState);
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// WiFi Upload screen (delegated to ui/ui_update)
// ═══════════════════════════════════════════════════════════════════

static void drawWifiScreen() {
    ui_wifi_draw();
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// WiFi Setup screen (on-device scanner + selection)
// ═══════════════════════════════════════════════════════════════════

static void drawWifiSetupScreen() {
    ui_wifi_setup_draw();
    needsRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
// On-screen keyboard (used by WiFi Setup for SSID/password entry)
// ═══════════════════════════════════════════════════════════════════

static void drawKeyboardScreen() {
    ui_keyboard_draw();
    needsRedraw = false;
}


// ═══════════════════════════════════════════════════════════════════
// Touch handlers
// ═══════════════════════════════════════════════════════════════════

static void handleLibraryTouch(int x, int y) {
    // Pass libraryFilter directly — no more int round-trip.
    AppState newState = ui_library_touch(x, y, books, libraryScroll, libraryFilter, filteredIndices);

    if (newState == STATE_READER) {
        // First draw after opening a book: use medium refresh for cleaner display
        readerRefresh.fastRefresh = true;
        readerRefresh.pageTurnsSinceFull = settings_get().refreshEveryPages - 1;  // triggers medium refresh
        appState = STATE_READER;
        needsRedraw = true;
        return;
    }

    if (newState == STATE_SETTINGS) {
        settingsSoftRefreshOnce = true;
        appState = STATE_SETTINGS;
        needsRedraw = true;
        return;
    }

    needsRedraw = true;
}

static void handleReaderTouch(int x, int y, bool isLongPress) {
    appState = ui_reader_touch(x, y, isLongPress, reader, readerRefresh);
    needsRedraw = true;
}

static void handleMenuTouch(int x, int y) {
    AppState newState = ui_reader_menu_touch(x, y, reader, readerRefresh);
    if (newState == STATE_TOC) tocScroll = 0;
    if (newState == STATE_BOOKMARKS) bmScroll = 0;
    appState = newState;
    needsRedraw = true;
}

static void handleGotoTouch(int x, int y) {
    appState = ui_reader_goto_touch(x, y, reader, readerRefresh);
    needsRedraw = true;
}

static void handleTocTouch(int x, int y) {
    appState = ui_reader_toc_touch(x, y, reader, tocScroll, readerRefresh);
    needsRedraw = true;
}

static void handleBookmarksTouch(int x, int y) {
    appState = ui_reader_bookmarks_touch(x, y, reader, bmScroll, readerRefresh);
    needsRedraw = true;
}

static void handleSettingsTouch(int x, int y) {
    AppState newState = ui_settings_touch(x, y, reader);
    if (newState == STATE_WIFI) {
        // Redirect to on-device WiFi Setup when no SSID has been configured —
        // the upload screen would otherwise immediately fail with
        // "No WiFi configured" and force a trip back here.
        if (settings_get().wifiSSID.length() == 0) {
            newState = STATE_WIFI_SETUP;
            // Falls through to the STATE_WIFI_SETUP branch below — single
            // ui_wifi_setup_open() call, single async scan kicked off.
        } else {
            wifi_upload_start();
            lightSleepEnabled = false;  // disable light sleep during WiFi
        }
    }
    if (newState == STATE_WIFI_SETUP) {
        ui_wifi_setup_open();
        lightSleepEnabled = false;  // keep CPU responsive during scan
    }
    if (newState == STATE_OTA_CHECK) {
        lightSleepEnabled = false;  // disable light sleep during OTA
        otaState.phase = OTA_CHECKING;
        otaState.updateAvailable = false;
        otaState.latestVersion = "";
    }
    // Note: do NOT re-arm settingsSoftRefreshOnce here for STATE_SETTINGS.
    // handleSettingsTouch fires while we are *already* in settings, so a
    // return value of STATE_SETTINGS just means "stay here".  Forcing a
    // medium refresh on every value tap defeated the partial-update path
    // and caused full-screen flashes when changing Font Size etc.
    if (newState == STATE_READER) {
        readerRefresh.fastRefresh = false;
    }
    appState = newState;
    needsRedraw = true;
}

static void handleOtaTouch(int x, int y) {
    AppState newState = ui_ota_touch(x, y, otaState);
    if (newState != STATE_OTA_CHECK && newState != STATE_WIFI) {
        // Treat this transition as fresh activity so we don't immediately
        // re-enter the idle path after a long OTA check.
        lightSleepEnabled = false;
        lastTouchOrButtonTime = millis();
    }
    if (newState == STATE_SETTINGS) {
        settingsSoftRefreshOnce = true;
    }
    appState = newState;
    needsRedraw = true;
}

static void handleWifiTouch(int x, int y) {
    AppState newState = ui_wifi_touch(x, y, books, filteredIndices);
    updateFilteredIndices();
    if (newState != STATE_WIFI && newState != STATE_OTA_CHECK) {
        // Reset idle timing after long blocking WiFi flows so the next screen
        // stays interactive.
        lightSleepEnabled = false;
        lastTouchOrButtonTime = millis();
    }
    if (newState == STATE_SETTINGS) {
        settingsSoftRefreshOnce = true;
    }
    appState = newState;
    needsRedraw = true;
}

static void handleWifiSetupTouch(int x, int y) {
    AppState newState = ui_wifi_setup_touch(x, y);
    if (newState == STATE_SETTINGS) {
        settingsSoftRefreshOnce = true;
        lightSleepEnabled = false;
        lastTouchOrButtonTime = millis();
    }
    appState = newState;
    needsRedraw = true;
}

static void handleKeyboardTouch(int x, int y) {
    AppState newState = ui_keyboard_touch(x, y);
    appState = newState;
    needsRedraw = true;
}

// ═══════════════════════════════════════════════════════════════════
// Deep sleep
// ═══════════════════════════════════════════════════════════════════

static void enterDeepSleep(bool triggeredByButton) {
    if (Serial) Serial.println("Entering deep sleep...");

    // Save progress whenever a book is open, regardless of which sub-state
    // (reader, menu, TOC, bookmarks, settings) we're currently in. Critical
    // path — if this fails the user wakes up at the wrong page; surface it.
    // BUT skip the save when the current chapter rendered the "[Could not
    // load …]" fallback — saving in that state traps the user at the
    // unloadable position permanently (Bug Nest A).
    if (reader.isOpen()) {
        if (reader.lastChapterLoadFailed()) {
            Serial.println("Sleep: skipping progress save (chapter load failed)");
        } else {
            reader.updateReadingTime();
            if (!reader.saveProgress()) {
                Serial.println("Sleep: progress save FAILED — position may be lost");
            }
        }
    }
    if (wifi_upload_running()) {
        wifi_upload_stop();
    }
    // Tear down the on-device WiFi scanner if it's still active.
    ui_wifi_setup_close();

    // Persist app state so wake can resume where we left off.
    // Reader sub-screens (menu/TOC/bookmarks) resume to reader.
    // Settings/WiFi/OTA resume to library (transient screens).
    {
        Preferences prefs;
        prefs.begin("ereader", false);

        int resumeState = (int)appState;
        // Collapse reader overlay states back to reader
        if (appState == STATE_MENU || appState == STATE_GOTO || appState == STATE_TOC || appState == STATE_BOOKMARKS) {
            resumeState = (int)STATE_READER;
        }
        // Transient states resume to library
        if (appState == STATE_WIFI || appState == STATE_OTA_CHECK || appState == STATE_SETTINGS ||
            appState == STATE_WIFI_SETUP || appState == STATE_WIFI_KEYBOARD) {
            resumeState = (int)STATE_LIBRARY;
        }
        prefs.putInt("sleepState", resumeState);
        prefs.putInt("sleepLibScrl", libraryScroll);

        // Persist open book filepath so we can reopen it on wake
        if (reader.getFilepath().length() > 0) {
            prefs.putString("sleepBook", reader.getFilepath());
        } else {
            prefs.putString("sleepBook", "");
        }

        // Book is stable at sleep time — reset crash guard
        prefs.putInt("crashCount", 0);

        if (Serial) {
            Serial.printf("Sleep: saved state=%d libScrl=%d book=%s\n",
                resumeState, libraryScroll, reader.getFilepath().c_str());
        }
        prefs.end();
    }

    // CRITICAL FIX: Single bounded wait for button release with defensive timeout.
    // Button is active LOW, so we wait for HIGH (released) state.
    if (triggeredByButton) {
        if (Serial) Serial.println("Sleep: waiting for button release (max 1s)...");
        unsigned long waitStart = millis();
        // Wait for HIGH (button released) with 1s timeout
        while (digitalRead(BUTTON_PIN) == LOW && (millis() - waitStart < 1000)) {
            delay(5);  // Faster polling for more responsive check
        }
        if (digitalRead(BUTTON_PIN) == LOW) {
            if (Serial) Serial.println("Sleep: button still LOW after 1s, proceeding anyway");
        } else {
            if (Serial) Serial.println("Sleep: button released");
        }
        delay(50);  // Brief settle time
    }

    // Only flush Serial if USB is connected (avoid blocking when disconnected)
    if (Serial) {
        Serial.println("Sleep: showing sleep image...");
        Serial.flush();
    }

    // Show sleep image (or default screen if no images found)
    sleep_image_show_next();

    // Frontlight off before deep sleep (Pro target only — no-op otherwise)
    frontlight_off();

    if (Serial) {
        Serial.println("Sleep: display updated");
        Serial.flush();
    }

    delay(50);  // Brief settle time

    // CRITICAL: Disable GPIO wakeup source configured for light sleep.
    // If we don't clear this, the touch INT pin (GPIO 47) can trigger an
    // immediate wake because GPIO wake sources persist across sleep types.
    // This was the root cause of the "immediate restart" bug after refactoring.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    // BUTTON_PIN = GPIO 0 (BOOT). ext1 mit ALL_LOW als Wake-Source.
    // Wake when button goes LOW (active LOW, pressed)
    // ESP_EXT1_WAKEUP_ALL_LOW: wake when ALL configured pins are LOW
    // Since we only have one pin, this effectively means "wake when this pin is LOW"
    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_PIN, ESP_EXT1_WAKEUP_ALL_LOW);

    if (Serial) {
        Serial.println("Sleep: entering deep sleep now");
        Serial.flush();
    }
    delay(50);  // Brief settle time (reduced since we're not flushing)
    
    // Hold GPIO pins in stable state before sleep
    gpio_pullup_en((gpio_num_t)BUTTON_PIN);
    gpio_pulldown_dis((gpio_num_t)BUTTON_PIN);
    
    // Feed watchdog before deep sleep to ensure clean state
    esp_task_wdt_reset();
    
    esp_deep_sleep_start();
}

// ═══════════════════════════════════════════════════════════════════
// Light Sleep — idle power reduction (~40mA → ~2mA)
// ═══════════════════════════════════════════════════════════════════

static bool lightSleepConfigured = false;
static const unsigned long LIGHT_SLEEP_IDLE_MS = 500; // idle before sleeping

static void configureLightSleep() {
    if (lightSleepConfigured) return;

    // TEMPORARILY DISABLED - debugging button/sleep issue
    // gpio_wakeup_enable((gpio_num_t)TOUCH_INT_PIN, GPIO_INTR_LOW_LEVEL);
    // esp_sleep_enable_gpio_wakeup();
    // gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);

    lightSleepConfigured = true;
    Serial.println("Light sleep DISABLED for debugging");
}

static bool canEnterLightSleep() {
    // Don't sleep during WiFi-active states
    if (appState == STATE_WIFI || appState == STATE_OTA_CHECK ||
        appState == STATE_WIFI_SETUP || appState == STATE_WIFI_KEYBOARD) {
        return false;
    }

    // Don't sleep if WiFi is connected (upload/OTA in progress)
    if (wifi_upload_running()) return false;

    // Don't sleep if light sleep is explicitly disabled
    if (!lightSleepEnabled) return false;

    // Don't sleep in the first 3 seconds after boot (let display settle)
    if (millis() - bootTime < 3000) return false;

    // Don't sleep if there's a pending redraw
    if (needsRedraw) return false;

    // Don't sleep if we just had input recently
    if (millis() - lastTouchOrButtonTime < LIGHT_SLEEP_IDLE_MS) return false;

    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Arduino setup/loop
// ═══════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    // No unconditional delay() here — the previous 500 ms wait was a
    // host-terminal settle hack that costs every cold-boot user half a
    // second. If you need it for development, gate behind a build flag.
    Serial.println("\n=== T5 E-Reader Firmware (Portrait) ===");
    debug_trace_boot_report();
    debug_trace_mark("setup:start");

    // Pro: SX1262 und GPS in sicheren Aus-Zustand zwingen, BEVOR der
    // gemeinsame SPI-Bus für die SD-Karte aufgebaut wird. Auf anderen
    // Targets ein No-Op.
    hw_disable_unused_init();

    esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
    bool wakingFromSleep = (wakeReason == ESP_SLEEP_WAKEUP_EXT1 ||
                            wakeReason == ESP_SLEEP_WAKEUP_TIMER);
    Serial.printf("Wakeup cause: %d (fromSleep=%d)\n", (int)wakeReason, wakingFromSleep);

    // Top button is sleep / wake; middle button is for page turns.
    // Both are active LOW.
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    // PAGE_BUTTON_PIN removed — top button (GPIO 21) handles all input
    // User button is read via PCA9535 IO expander; no direct GPIO pinMode.

    // Set wake cooldown if waking from sleep
    if (wakingFromSleep) {
        wakeCooldownEnd = millis() + 2000;  // No-sleep for 2s after wake
    }

    // Critical: wait for button release before proceeding when waking from sleep.
    // If we don't wait, the still-held button triggers immediate re-sleep.
    if (wakingFromSleep) {
        Serial.println("Wake: waiting for button release...");
        unsigned long waitStart = millis();
        while (digitalRead(BUTTON_PIN) == LOW && millis() - waitStart < 2000) {
            delay(10);
        }
        delay(100);  // Additional debounce
        Serial.println("Wake: button released, proceeding");
    }

    // Wire must own the I²C-0 driver BEFORE epdiy's display_init() runs.
    // epdiy's epd_board_v7 normally calls ESP_ERROR_CHECK(i2c_driver_install(...)),
    // but our patched copy (tools/patch_epdiy.py) tolerates ESP_ERR_INVALID_STATE
    // — so epdiy simply reuses Wire's driver instead of trying to install its own.
    // This lets BQ27220 + GT911 use plain Arduino Wire afterwards without conflict.
    Wire.begin(TOUCH_SDA, TOUCH_SCL);

    debug_trace_mark("setup:before_display_init");
    display_init();
    debug_trace_mark("setup:after_display_init");


    if (wakingFromSleep) {
        showWakeFeedback();
    }

    if (!wakingFromSleep) {
        // Cold boot: show splash screen.
        display_clear();
        drawSplashScreen();
        display_update();
    }
    // Wake path: do NOT call display_clear() — the sleep image is still
    // latched on the panel.  The first drawXxxScreen() call will overwrite
    // the framebuffer and push a full refresh, which cleanly replaces the
    // sleep image with the restored UI.

    debug_trace_mark("setup:before_battery_init");
    battery_init();
    debug_trace_mark("setup:after_battery_init");
    Serial.printf("Battery: %.2fV (%d%%)\n", battery_voltage(), battery_percent());

    debug_trace_mark("setup:before_touch_init");
    if (!touch_init()) {
        Serial.println("WARNING: Touch not available");
    }
    debug_trace_mark("setup:after_touch_init");

    debug_trace_mark("setup:before_library_init");
    if (!library_init()) {
        display_set_font_size(2);  // error screen in UI font
        display_fill_screen(15);
        const char* err = "SD Card Error!";
        int ew = display_text_width(err);
        display_draw_text((W - ew) / 2, H / 2 - 20, err, 0);
        const char* hint = "Insert SD card and restart";
        int hw = display_text_width(hint);
        display_draw_text((W - hw) / 2, H / 2 + 40, hint, 6);
        display_update();
        // Halt here. Continuing into settings_init / library_scan against an
        // unmounted SD silently produces a fake-but-empty library and burns
        // through the boot path with no working storage. Wait for a power
        // cycle / SD insertion + reset.
        while (true) {
            esp_task_wdt_reset();
            delay(1000);
        }
    }

    debug_trace_mark("setup:after_library_init");
    settings_init();
    debug_trace_mark("setup:after_settings_init");
    frontlight_init();
    frontlight_apply_from_settings();
    debug_trace_mark("setup:after_frontlight_init");
    // Pre-warm reader-font selection so the first reader open does not
    // pay the glyph-cache invalidation cost mid-render.  The next
    // ui_*_draw call switches back to Inter for chrome.
    display_set_font(settings_get().fontSizeLevel, settings_get().fontFamily);
    display_set_font_size(2);  // boot ends on UI font (Inter)
    debug_trace_mark("setup:before_library_scan");
    books = library_scan();
    debug_trace_mark("setup:after_library_scan", String(books.size()));
    updateFilteredIndices();
    cover_cache_clear();
    wifi_upload_init();
    wifi_upload_set_reader(&reader);

    // Restore state from before deep sleep, or default to library
    if (wakingFromSleep) {
        Preferences prefs;
        prefs.begin("ereader", true);  // read-only
        int savedState = prefs.getInt("sleepState", (int)STATE_LIBRARY);
        libraryScroll = prefs.getInt("sleepLibScrl", 0);
        String savedBook = prefs.getString("sleepBook", "");
        prefs.end();

        // Guard against a corrupted / future-firmware NVS value landing us in
        // an undefined AppState (the draw/touch switches would hit `default`
        // and leave the screen frozen with no recovery path).
        const int kMaxKnownState = (int)STATE_WIFI_KEYBOARD;
        if (savedState < 0 || savedState > kMaxKnownState ||
            savedState == (int)STATE_BOOT) {
            Serial.printf("Wake: invalid savedState=%d, falling back to library\n", savedState);
            savedState = (int)STATE_LIBRARY;
        }

        Serial.printf("Wake: restoring state=%d libScrl=%d book=%s\n",
            savedState, libraryScroll, savedBook.c_str());

        // Clamp libraryScroll to valid range
        if (libraryScroll < 0) libraryScroll = 0;
        if (libraryScroll >= (int)books.size()) libraryScroll = 0;

        bool resumedReader = false;
        if (savedState == (int)STATE_READER && savedBook.length() > 0) {
            debug_trace_mark("wake:before_openBook", savedBook);
            // ── Crash guard: increment count BEFORE the risky openBook call ──
            {
                Preferences cgPrefs;
                cgPrefs.begin("ereader", false);
                int crashCount = cgPrefs.getInt("crashCount", 0);
                crashCount++;
                cgPrefs.putInt("crashCount", crashCount);
                cgPrefs.end();

                if (crashCount >= 2) {
                    Serial.println("Crash guard triggered: clearing saved book");
                    Preferences clrPrefs;
                    clrPrefs.begin("ereader", false);
                    clrPrefs.putString("sleepBook", "");
                    clrPrefs.putInt("crashCount", 0);
                    // Sticky flag picked up by the library's first draw to
                    // surface a one-time "previous session crashed" toast.
                    clrPrefs.putBool("crashRcvd", true);
                    clrPrefs.end();
                    savedBook = "";
                }
            }

            if (savedBook.length() > 0) {
                // Reopen the book — openBook() calls loadProgress() internally,
                // which restores chapter + page from the progress JSON on SD.
                if (reader.openBook(savedBook.c_str())) {
                    debug_trace_mark("wake:after_openBook", savedBook);
                    // Book opened successfully — reset crash guard
                    Preferences okPrefs;
                    okPrefs.begin("ereader", false);
                    okPrefs.putInt("crashCount", 0);
                    okPrefs.end();

                    appState = STATE_READER;
                    readerRefresh.fastRefresh = false;
                    readerRefresh.forceFullRefresh = true;
                    readerRefresh.pageTurnsSinceFull = 0;
                    resumedReader = true;
                    Serial.printf("Wake: resumed reader — %s ch%d pg%d\n",
                        reader.getTitle().c_str(),
                        reader.getCurrentChapter(), reader.getCurrentPage());
                } else {
                    Serial.printf("Wake: failed to reopen %s, falling back to library\n",
                        savedBook.c_str());
                }
            }
        }

        if (!resumedReader) {
            appState = STATE_LIBRARY;
        }
        debug_trace_mark("wake:post_restore", String((int)appState));

        // Draw the restored screen immediately after the wake banner so the
        // user gets instant acknowledgement first, then the restored content.
        firstLibraryDraw = true;
        needsRedraw = true;
        if (appState == STATE_READER) {
            debug_trace_mark("wake:before_draw_reader");
            drawReaderScreen();
            debug_trace_mark("wake:after_draw_reader");
        } else {
            debug_trace_mark("wake:before_draw_library");
            drawLibraryScreen();
            debug_trace_mark("wake:after_draw_library");

            // Crash-guard banner — if the previous session failed to reopen
            // the book twice in a row, surface the recovery action.
            // Two-phase: read in a separate session, render, then clear.
            // A power-loss between read and render leaves the flag set so
            // the user still sees the banner on the next boot.
            bool crashRecovered = false;
            {
                Preferences crPrefs;
                crPrefs.begin("ereader", true);  // read-only
                crashRecovered = crPrefs.getBool("crashRcvd", false);
                crPrefs.end();
            }
            if (crashRecovered) {
                // Reuse the file-scope W/H constants (same values as
                // display_width()/display_height()).
                const int bh = 70;
                const int by = H / 2 - bh / 2;
                display_draw_filled_rect(0, by, W, bh, 2);
                const char* msg = "Previous session crashed — book position reset.";
                int mw = display_text_width(msg);
                display_draw_text((W - mw) / 2, by + bh / 2 + 8, msg, 15);
                display_update_fast();
                // Clear ONLY after the banner is on the panel.
                Preferences crPrefs;
                crPrefs.begin("ereader", false);
                crPrefs.putBool("crashRcvd", false);
                crPrefs.end();
            }
        }
        needsRedraw = false;
        Serial.println("Wake: immediate screen draw complete");
    } else {
        appState = STATE_LIBRARY;
        needsRedraw = true;
    }

    lastActivity = millis();
    lastTouchOrButtonTime = millis();
    bootTime = millis();

    // Configure light sleep with GPIO wakeup
    configureLightSleep();

    debug_trace_mark("setup:complete");
    Serial.println("Setup complete");
}

// Helper: advance or go back one page via the physical button
static void buttonPageForward() {
    lastActivity = millis();
    if (appState == STATE_READER) {
        if (reader.nextPage()) {
            readerRefresh.fastRefresh = !reader.didChapterChange();
            if (reader.didChapterChange()) readerRefresh.chapterJump = true;
            needsRedraw = true;
        }
    } else if (appState == STATE_LIBRARY && !filteredIndices.empty()) {
        const Settings& s = settings_get();
        int numVis = (int)filteredIndices.size();
        int listStartY = HEADER_HEIGHT + FILTER_TAB_H + MARGIN_Y;
        if (library_find_current_book(books) >= 0 && libraryFilter == FILTER_ALL) listStartY += FONT_H + 20;
        int itemsPerPage;
        if (s.libraryViewMode == 1) {
            int posterH = 310;
            int rowsVisible = max(1, (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / (posterH + 14));
            itemsPerPage = rowsVisible * 2;
        } else {
            itemsPerPage = (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / BOOK_ITEM_H;
        }
        if (libraryScroll + itemsPerPage < numVis) {
            libraryScroll += itemsPerPage;
            needsRedraw = true;
        }
    }
}

static void buttonPageBackward() {
    lastActivity = millis();
    if (appState == STATE_READER) {
        if (reader.prevPage()) {
            readerRefresh.fastRefresh = !reader.didChapterChange();
            if (reader.didChapterChange()) readerRefresh.chapterJump = true;
            needsRedraw = true;
        }
    } else if (appState == STATE_LIBRARY && !filteredIndices.empty()) {
        const Settings& s = settings_get();
        int listStartY = HEADER_HEIGHT + FILTER_TAB_H + MARGIN_Y;
        if (library_find_current_book(books) >= 0 && libraryFilter == FILTER_ALL) listStartY += FONT_H + 20;
        int itemsPerPage;
        if (s.libraryViewMode == 1) {
            int posterH = 310;
            int rowsVisible = max(1, (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / (posterH + 14));
            itemsPerPage = rowsVisible * 2;
        } else {
            itemsPerPage = (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / BOOK_ITEM_H;
        }
        if (libraryScroll > 0) {
            libraryScroll -= itemsPerPage;
            if (libraryScroll < 0) libraryScroll = 0;
            needsRedraw = true;
        }
    }
}

#ifdef USER_BUTTON_VIA_PCA9535
// PCA9535 register addresses (port 1 = upper byte / IO10..IO17).
#define PCA9535_REG_CONFIG_PORT1  0x07

// Configure IO12 (port 1 bit 2) as input on the PCA9535. epdiy initializes
// most port-1 pins as outputs for the TPS65185 control lines, but IO12 on
// the T5S3 PRO board is wired to a physical button. We set just that bit
// to input via read-modify-write, leaving the other epdiy-managed pins as
// outputs.
static bool pca9535_user_button_configured = false;
static bool pca9535_configure_user_button_input() {
    Wire.beginTransmission(PCA9535_I2C_ADDR);
    Wire.write(PCA9535_REG_CONFIG_PORT1);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)PCA9535_I2C_ADDR, (uint8_t)1) != 1) return false;
    uint8_t cfg = Wire.read();
    uint8_t newCfg = cfg | (1 << PCA9535_USER_BUTTON_BIT);  // 1 = input
    if (newCfg == cfg) return true;  // already an input
    Wire.beginTransmission(PCA9535_I2C_ADDR);
    Wire.write(PCA9535_REG_CONFIG_PORT1);
    Wire.write(newCfg);
    if (Wire.endTransmission(true) != 0) return false;
    Serial.printf("PCA9535: IO12 config 0x%02X -> 0x%02X (set to input)\n", cfg, newCfg);
    return true;
}

// Read the user-button IO bit from the PCA9535 IO expander.
// Returns true when the button is pressed (active LOW on bit 2 of port 1).
// Returns false on I2C failure so a missing/unresponsive expander doesn't
// trigger spurious actions.
static bool pca9535_read_user_button() {
    if (!pca9535_user_button_configured) {
        if (pca9535_configure_user_button_input()) {
            pca9535_user_button_configured = true;
        }
    }
    Wire.beginTransmission(PCA9535_I2C_ADDR);
    Wire.write(PCA9535_REG_INPUT_PORT1);
    if (Wire.endTransmission(false) != 0) {
        static unsigned long lastErr = 0;
        if (millis() - lastErr > 2000) {
            Serial.println("PCA9535: addr write failed");
            lastErr = millis();
        }
        return false;
    }
    if (Wire.requestFrom((uint8_t)PCA9535_I2C_ADDR, (uint8_t)1) != 1) {
        static unsigned long lastErr = 0;
        if (millis() - lastErr > 2000) {
            Serial.println("PCA9535: read failed");
            lastErr = millis();
        }
        return false;
    }
    uint8_t v = Wire.read();
    return (v & (1 << PCA9535_USER_BUTTON_BIT)) == 0;  // active LOW
}
#endif

// Configurable user-button dispatcher. Called for tap/double/long gestures.
// Reads the per-gesture action from settings and routes.
void button_action_execute(uint8_t action) {
    lastActivity = millis();
    switch (action) {
        case BTN_ACTION_NONE:
            break;
        case BTN_ACTION_BACKLIGHT_TOGGLE: {
            Settings& s = settings_get();
            s.frontlightEnabled = !s.frontlightEnabled;
            frontlight_apply_from_settings();
            if (!settings_save()) Serial.println("Settings: save failed (button action)");
            break;
        }
        case BTN_ACTION_LIBRARY:
            if (appState != STATE_LIBRARY) {
                appState = STATE_LIBRARY;
                firstLibraryDraw = true;
                needsRedraw = true;
            }
            break;
        case BTN_ACTION_SLEEP:
            enterDeepSleep(true);
            break;
        case BTN_ACTION_NEXT_PAGE:
            buttonPageForward();
            break;
        case BTN_ACTION_PREV_PAGE:
            buttonPageBackward();
            break;
        case BTN_ACTION_MENU:
            if (appState == STATE_READER) {
                appState = STATE_MENU;
                readerRefresh.fastRefresh = false;
                needsRedraw = true;
            }
            break;
        default:
            break;
    }
}

void loop() {
    if (appState == STATE_WIFI) {
        wifi_upload_handle();

        // Periodic redraw during connection phase for animation.
        static unsigned long lastWifiRedraw = 0;
        if (wifi_upload_connecting() && millis() - lastWifiRedraw >= 500) {
            needsRedraw = true;
            lastWifiRedraw = millis();
        }

        // One-shot redraw on connecting → running transition so the screen
        // actually paints the IP/QR view instead of leaving the last
        // "Connecting…" frame on the panel.
        static bool shownWifiRunning = false;
        if (wifi_upload_running()) {
            if (!shownWifiRunning) {
                needsRedraw = true;
                shownWifiRunning = true;
            }
        } else {
            shownWifiRunning = false;
        }

        // Also redraw once on error state transition.
        static bool shownWifiError = false;
        if (wifi_upload_has_error()) {
            if (!shownWifiError) {
                needsRedraw = true;
                shownWifiError = true;
            }
        } else {
            shownWifiError = false;
        }
    }

    // Poll boot button (GPIO 0): gesture mapping via settings (bootButton*).
    // Long-press always sleeps when disabled — fail-safe so the device can
    // always be powered down without going through the UI.
    {
        const Settings& sBtn = settings_get();
        bool btnPressed = (digitalRead(BUTTON_PIN) == LOW);
        if (btnPressed && !btnWasPressed) {
            // Fresh press
            btnDownTime = millis();
            btnLongFired = false;
            lastTouchOrButtonTime = millis();
        } else if (btnPressed && btnWasPressed) {
            // Still held — check for long-press trigger
            unsigned long heldMs = millis() - btnDownTime;
            if (!btnLongFired && heldMs >= BUTTON_POWER_MS &&
                millis() >= wakeCooldownEnd) {
                btnLongFired = true;
                btnPressCount = 0;
                uint8_t act = sBtn.bootButtonEnabled
                              ? sBtn.bootButtonLongAction
                              : (uint8_t)BTN_ACTION_SLEEP;
                Serial.printf("Boot button long-press — action=%u\n", (unsigned)act);
                button_action_execute(act);
            }
        } else if (!btnPressed && btnWasPressed) {
            // Released — only count as tap if it wasn't a long-press
            unsigned long heldMs = millis() - btnDownTime;
            if (!btnLongFired &&
                heldMs >= BUTTON_DEBOUNCE_MS && heldMs < BUTTON_POWER_MS) {
                btnPressCount++;
                lastBtnReleaseTime = millis();
            }
        }
        btnWasPressed = btnPressed;

        // Resolve single vs double press after the window expires.
        // Tap/double only fire when the button is enabled — long-press is
        // handled above as a fail-safe regardless.
        if (btnPressCount > 0 && !btnPressed &&
            (millis() - lastBtnReleaseTime >= DOUBLE_PRESS_WINDOW_MS)) {
            if (sBtn.bootButtonEnabled) {
                if (btnPressCount >= 2) {
                    Serial.println("Boot button double-press");
                    button_action_execute(sBtn.bootButtonDoubleAction);
                } else {
                    Serial.println("Boot button single-press");
                    button_action_execute(sBtn.bootButtonTapAction);
                }
            }
            btnPressCount = 0;
        }
    }

#ifdef USER_BUTTON_VIA_PCA9535
    // Configurable user button — physical button wired to PCA9535 IO12,
    // read via I2C. Polling only runs when the user has explicitly enabled
    // it in settings. The PCA9535 is shared with epdiy (TPS65185 control
    // lines) so I2C contention is possible; we throttle reads to ~30Hz to
    // keep bus pressure low.
    {
        const Settings& s = settings_get();
        static unsigned long lastUbtnPoll = 0;
        static int8_t lastEnableLog = -1;
        if ((int8_t)s.userButtonEnabled != lastEnableLog) {
            Serial.printf("UserButton polling: enabled=%d\n", (int)s.userButtonEnabled);
            lastEnableLog = (int8_t)s.userButtonEnabled;
            // Re-run presence detection + PCA9535 config when the user toggles
            // the feature, so disabling then re-enabling can recover from a
            // prior stuck-LOW classification.
            ubtnInitDone = false;
            ubtnInitStart = 0;
            ubtnPresent = false;
            pca9535_user_button_configured = false;
        }
        unsigned long ubtnNow = millis();
        if (s.userButtonEnabled && ubtnNow - lastUbtnPoll >= 30) {
            lastUbtnPoll = ubtnNow;
            bool ubtnPressed = pca9535_read_user_button();

            if (!ubtnInitDone) {
                if (ubtnInitStart == 0) ubtnInitStart = millis();
                if (!ubtnPressed) {
                    ubtnPresent = true;
                    ubtnInitDone = true;
                } else if (millis() - ubtnInitStart > 1000) {
                    ubtnPresent = false;
                    ubtnInitDone = true;
                    Serial.println("User button: pin stuck LOW, disabling gesture handler");
                }
            }

            if (ubtnPresent) {
                if (ubtnPressed && !ubtnWasPressed) {
                    ubtnDownTime = millis();
                    ubtnLongFired = false;
                    lastTouchOrButtonTime = millis();
                } else if (ubtnPressed && ubtnWasPressed) {
                    unsigned long heldMs = millis() - ubtnDownTime;
                    if (!ubtnLongFired && heldMs >= BUTTON_POWER_MS &&
                        millis() >= wakeCooldownEnd) {
                        ubtnLongFired = true;
                        ubtnPressCount = 0;
                        Serial.println("User button long-press");
                        button_action_execute(s.userButtonLongAction);
                    }
                } else if (!ubtnPressed && ubtnWasPressed) {
                    unsigned long heldMs = millis() - ubtnDownTime;
                    if (!ubtnLongFired &&
                        heldMs >= BUTTON_DEBOUNCE_MS && heldMs < BUTTON_POWER_MS) {
                        ubtnPressCount++;
                        ubtnLastReleaseTime = millis();
                    }
                }
                ubtnWasPressed = ubtnPressed;

                if (ubtnPressCount > 0 && !ubtnPressed &&
                    (millis() - ubtnLastReleaseTime >= DOUBLE_PRESS_WINDOW_MS)) {
                    if (ubtnPressCount >= 2) {
                        Serial.println("User button double-press");
                        button_action_execute(s.userButtonDoubleAction);
                    } else {
                        Serial.println("User button single-press");
                        button_action_execute(s.userButtonTapAction);
                    }
                    ubtnPressCount = 0;
                }
            } else {
                ubtnWasPressed = ubtnPressed;
            }
        }
    }
#endif

    // Poll touch with long-press detection
    static bool lastTouchState = false;
    static TouchPoint lastTouchPt;
    static TouchPoint movePt;          // last position seen while finger down

    TouchPoint currentPt;
    bool currentTouch = touch_read(currentPt);

    if (currentTouch && !lastTouchState) {
        // Touch down
        lastTouchPt = currentPt;
        movePt = currentPt;
        touchDownTime = millis();
        touchHandled = false;
        lastTouchOrButtonTime = millis();
    } else if (currentTouch && lastTouchState) {
        // Touch held — keep awake AND record latest position so swipe
        // detection on release has a real endpoint.
        movePt = currentPt;
        lastTouchOrButtonTime = millis();
    } else if (!currentTouch && lastTouchState) {
        // Touch up — process tap or swipe
        lastActivity = millis();

        if (!touchHandled) {
            int tx = lastTouchPt.x;
            int ty = lastTouchPt.y;
            // Use the last live position observed while held — touch_read()
            // typically returns false on the actual release, so currentPt
            // would be stale; movePt was updated each held-tick.
            int dx = movePt.x - lastTouchPt.x;
            int dy = movePt.y - lastTouchPt.y;
            int absDx = dx < 0 ? -dx : dx;
            int absDy = dy < 0 ? -dy : dy;
            unsigned long duration = millis() - touchDownTime;
            bool isLongPress = (duration >= LONG_PRESS_MS);

            // Detect horizontal swipe in reader and library modes. Threshold
            // tuned for an intentional gesture — small drift on a tap won't
            // trigger.
            bool swipeHandled = false;
            if (absDx > 60 && absDy < 80) {
                if (appState == STATE_READER) {
                    if (dx < 0) {
                        // Swipe left → next page
                        if (reader.nextPage()) {
                            readerRefresh.fastRefresh = !reader.didChapterChange();
                            if (reader.didChapterChange()) readerRefresh.chapterJump = true;
                            needsRedraw = true;
                        }
                        swipeHandled = true;
                    } else {
                        // Swipe right → prev page
                        if (reader.prevPage()) {
                            readerRefresh.fastRefresh = !reader.didChapterChange();
                            if (reader.didChapterChange()) readerRefresh.chapterJump = true;
                            needsRedraw = true;
                        }
                        swipeHandled = true;
                    }
                } else if (appState == STATE_LIBRARY && !filteredIndices.empty()) {
                    // Library swipe: compute items per page for current view mode
                    const Settings& s = settings_get();
                    int numVis = (int)filteredIndices.size();
                    int itemsPerPage;
                    int listStartY = HEADER_HEIGHT + FILTER_TAB_H + MARGIN_Y;
                    if (library_find_current_book(books) >= 0 && libraryFilter == FILTER_ALL) listStartY += FONT_H + 20;
                    if (s.libraryViewMode == 1) {
                        int posterH = 310;
                        int rowsVisible = max(1, (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / (posterH + 14));
                        itemsPerPage = rowsVisible * 2;
                    } else {
                        itemsPerPage = (H - listStartY - FOOTER_HEIGHT - MARGIN_Y) / BOOK_ITEM_H;
                    }
                    if (dx < 0) {
                        if (libraryScroll + itemsPerPage < numVis) {
                            libraryScroll += itemsPerPage;
                            needsRedraw = true;
                        }
                        swipeHandled = true;
                    } else {
                        if (libraryScroll > 0) {
                            libraryScroll -= itemsPerPage;
                            if (libraryScroll < 0) libraryScroll = 0;
                            needsRedraw = true;
                        }
                        swipeHandled = true;
                    }
                }
            }

            // Fall back to tap handling if not a swipe
            if (!swipeHandled) {
                switch (appState) {
                    case STATE_LIBRARY:   handleLibraryTouch(tx, ty);           break;
                    case STATE_READER:    handleReaderTouch(tx, ty, isLongPress); break;
                    case STATE_MENU:      handleMenuTouch(tx, ty);              break;
                    case STATE_GOTO:      handleGotoTouch(tx, ty);              break;
                    case STATE_TOC:       handleTocTouch(tx, ty);               break;
                    case STATE_BOOKMARKS: handleBookmarksTouch(tx, ty);         break;
                    case STATE_SETTINGS:  handleSettingsTouch(tx, ty);          break;
                    case STATE_OTA_CHECK: handleOtaTouch(tx, ty);              break;
                    case STATE_WIFI:      handleWifiTouch(tx, ty);              break;
                    case STATE_WIFI_SETUP:    handleWifiSetupTouch(tx, ty);     break;
                    case STATE_WIFI_KEYBOARD: handleKeyboardTouch(tx, ty);      break;
                    case STATE_BOOT:
                        // Splash phase — touches are deliberately ignored
                        // until setup() finishes the state transition.
                        break;
                    default: break;
                }
            }
        }
    }
    lastTouchState = currentTouch;

    if (appState == STATE_OTA_CHECK && otaState.phase == OTA_CHECKING && !needsRedraw) {
        ui_ota_tick(otaState);
        needsRedraw = true;
    }

    if (appState == STATE_WIFI_SETUP) {
        ui_wifi_setup_tick();
        if (ui_wifi_setup_dirty()) needsRedraw = true;
    }

    // Redraw if needed
    if (needsRedraw) {
        switch (appState) {
            case STATE_LIBRARY:   drawLibraryScreen();   break;
            case STATE_READER:    drawReaderScreen();    break;
            case STATE_MENU:      drawMenuOverlay();     break;
            case STATE_GOTO:      drawGotoScreen();      break;
            case STATE_TOC:       drawTocScreen();       break;
            case STATE_BOOKMARKS: drawBookmarksScreen(); break;
            case STATE_SETTINGS:  drawSettingsScreen();  break;
            case STATE_OTA_CHECK: drawOtaScreen();       break;
            case STATE_WIFI:      drawWifiScreen();      break;
            case STATE_WIFI_SETUP:    drawWifiSetupScreen(); break;
            case STATE_WIFI_KEYBOARD: drawKeyboardScreen();  break;
            case STATE_BOOT:
                // Splash already painted by drawSplashScreen() in setup();
                // nothing to redraw here.
                break;
            default: break;
        }
    }

    // Pre-caching removed — lines are stored on SD card now

    // Deep sleep check — defer while WiFi flows are active so a long-idle
    // upload session isn't silently killed mid-transfer.
    unsigned long sleepMs = (unsigned long)settings_get().sleepTimeoutMin * 60UL * 1000UL;
    bool wifiActive = wifi_upload_running() || wifi_upload_connecting() ||
                      appState == STATE_WIFI || appState == STATE_WIFI_SETUP ||
                      appState == STATE_WIFI_KEYBOARD || appState == STATE_OTA_CHECK;
    if (wifiActive) lastActivity = millis();
    if (millis() - lastActivity > sleepMs) {
        enterDeepSleep();
    }

    // Light sleep instead of busy-wait delay when idle
    if (canEnterLightSleep()) {
        esp_light_sleep_start();
        // Woke up from light sleep — immediately continue to poll touch/button
    } else {
        delay(TOUCH_POLL_MS);
    }
}

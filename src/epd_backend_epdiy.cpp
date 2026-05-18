// src/epd_backend_epdiy.cpp
//
// Backend implementation against upstream vroland/epdiy v2.0.0. Used by the
// `lilygo-t5s3-pro` build env. Pro hardware uses the V7 PMIC topology
// (TPS65185 via PCA9555 on I2C bus 39/40), which upstream-epdiy supports
// out of the box via its `epd_board_v7` definition. VCOM=1560 mV per the
// reference example in the LilyGo T5S3-4.7-e-paper-PRO repo.
//
// Option A from the design: backend holds its own internal high-level
// framebuffer; display.cpp's _lfb is copied in on each draw call via
// epd_copy_to_framebuffer().
//
// IMPORTANT — type name collision:
//   Our host header epd_backend.h defines `typedef struct { ... } EpdRect;`
//   in extern "C". Upstream epdiy.h also defines `typedef struct { ... } EpdRect;`
//   in extern "C". The two structs are layout-compatible (both are
//   { int/int32_t x, y, width, height }, 16 bytes total on ESP32-S3) but
//   the C type system would still flag them as a redefinition error.
//   We resolve this by *renaming* our host type to `HostEpdRect` inside this
//   translation unit via a preprocessor #define around the host-header include.
//   Function signatures end up declaring/defining `HostEpdRect` parameters,
//   but at the ABI level the calls from display.cpp (which sees `EpdRect`)
//   are byte-for-byte identical.
#include <Arduino.h>  // millis()
#include <string.h>   // memset for direct hl_state buffer reset
#include <cstddef>    // offsetof for ABI-layout static_asserts
#include "esp_task_wdt.h"

#define EpdRect HostEpdRect
#include "epd_backend.h"
#undef EpdRect

extern "C" {
#include "epdiy.h"
#include "epd_highlevel.h"
}

// Confirm ABI compatibility between the two rect types at compile time.
// sizeof guards against added/removed/padded fields; the four offsetof
// asserts below additionally guard against field reordering — an upstream
// epdiy refactor that, say, swapped width/height would still compile and
// silently corrupt every draw call. Catch it at the compiler instead.
static_assert(sizeof(HostEpdRect) == sizeof(::EpdRect),
              "HostEpdRect / epdiy EpdRect size mismatch");
static_assert(offsetof(HostEpdRect, x)      == offsetof(::EpdRect, x),
              "HostEpdRect / epdiy EpdRect: field x offset mismatch");
static_assert(offsetof(HostEpdRect, y)      == offsetof(::EpdRect, y),
              "HostEpdRect / epdiy EpdRect: field y offset mismatch");
static_assert(offsetof(HostEpdRect, width)  == offsetof(::EpdRect, width),
              "HostEpdRect / epdiy EpdRect: field width offset mismatch");
static_assert(offsetof(HostEpdRect, height) == offsetof(::EpdRect, height),
              "HostEpdRect / epdiy EpdRect: field height offset mismatch");

static EpdiyHighlevelState hl_state;

// Pro hardware VCOM (millivolts, absolute value; the chip applies negative).
// LilyGo's reference example uses 1560 mV (= -1.56 V) but that left text and
// dark UI backgrounds visibly washed out on our panel. Bumped to 2400 mV
// (= -2.40 V) for stronger contrast. Safe range for ED047TC1 is roughly
// 1500–2500 mV; tune within that window if the panel charge of a specific
// board behaves differently.
static constexpr uint16_t PRO_VCOM_MV = 2400;

// Fallback ambient temperature used when (a) the caller passes
// EPD_BE_DEFAULT_TEMP_C and we have no cached real reading yet, or (b) the
// TPS65185 temperature sensor returns out-of-range / fails. 25 °C matches
// the upstream epdiy examples and is the room-temperature design point of
// the waveform LUTs.
static constexpr int DEFAULT_TEMPERATURE_C = 25;

// Temperature cache. epd_ambient_temperature() does an I2C transaction on
// the TPS65185 every call (~1-2 ms); for partial-update bursts that fire
// many draws per second we amortize via a short TTL cache and reuse the
// last sane reading on transient sensor errors.
static constexpr unsigned long TEMPERATURE_CACHE_TTL_MS = 2000;
static int           s_cached_temp_c     = DEFAULT_TEMPERATURE_C;
static unsigned long s_temp_cache_at_ms  = 0;
static bool          s_temp_cache_valid  = false;

int epd_be_get_ambient_temp_cached() {
    const unsigned long now = millis();
    if (s_temp_cache_valid &&
        (now - s_temp_cache_at_ms) < TEMPERATURE_CACHE_TTL_MS) {
        return s_cached_temp_c;
    }

    // epd_ambient_temperature() returns float; the V7 board reads via the
    // TPS65185's on-chip sensor. On sensor failure upstream returns 0.0 or
    // 21.0 (no-sensor fallback) — both within range, so we additionally
    // sanity-clamp to a believable e-paper operating envelope.
    const float raw = epd_ambient_temperature();
    const int   t   = (int)raw;
    if (t >= -20 && t <= 70) {
        s_cached_temp_c = t;
    }
    // On out-of-range: keep the previous s_cached_temp_c (cold-boot init
    // value is DEFAULT_TEMPERATURE_C, so we never feed garbage downstream).
    s_temp_cache_at_ms = now;
    s_temp_cache_valid = true;
    return s_cached_temp_c;
}

// epdiy's main branch (ESP-IDF 5.x compatible) ships an EpdInitConfig
// header that lets us share an existing I2C bus. Older epdiy 2.0.0
// (used by espressif32@6.4.0 env) doesn't have this — we fall back to
// the legacy epd_init() + patch_epdiy.py workaround on that path.
#if __has_include(<epd_init_config.h>)
  #include <epd_init_config.h>
  #include <driver/i2c_master.h>
  #define PAPERLOOM_EPDIY_HAS_INIT_CONFIG 1
#endif

void epd_be_init() {
#if defined(PAPERLOOM_EPDIY_HAS_INIT_CONFIG)
    // pioarduino path: pass Wire's already-initialized I2C bus to epdiy
    // so epdiy doesn't try to create its own (which fails because Wire
    // already owns port 0).
    i2c_master_bus_handle_t shared_bus = NULL;
    esp_err_t err = i2c_master_get_bus_handle(I2C_NUM_0, &shared_bus);
    if (err == ESP_OK && shared_bus != NULL) {
        EpdI2cConfig i2c_cfg = { .bus_handle = shared_bus };
        EpdInitConfig init_cfg = { .i2c = &i2c_cfg };
        epd_init_with_config(&epd_board_v7, &ED047TC1, EPD_OPTIONS_DEFAULT, &init_cfg);
    } else {
        // Wire didn't init (yet?) — let epdiy try to own the bus.
        // This is unlikely; logs the issue for debugging.
        Serial.printf("[epd_be_init] WARN: i2c_master_get_bus_handle failed (%d), "
                      "falling back to epdiy bus ownership\n", err);
        epd_init(&epd_board_v7, &ED047TC1, EPD_OPTIONS_DEFAULT);
    }
#else
    // espressif32@6.4.0 path: legacy I2C driver, patch_epdiy.py handles
    // the double-install case.
    epd_init(&epd_board_v7, &ED047TC1, EPD_OPTIONS_DEFAULT);
#endif

    epd_set_vcom(PRO_VCOM_MV);
    // Force LCD pixel clock to a known rate so PCLK and CKV (RMT) are on the
    // same footing. epdiy@2.0.0 silently halves PCLK from 20→10 MHz when it
    // detects CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE < 64 (Arduino-ESP32 hardcodes
    // 32) — but the CKV RMT signal was already configured at 20 MHz, so each
    // scan line only receives half its pixel data before CKV advances. Result
    // on a fresh boot: half the panel stays at the previous (white) state.
    // Calling epd_set_lcd_pixel_clock_MHz here rebuilds the CKV signal at the
    // same rate as PCLK.
    epd_set_lcd_pixel_clock_MHz(8);
    hl_state = epd_hl_init(EPD_BUILTIN_WAVEFORM);
}

void epd_be_poweron()      { epd_poweron(); }
void epd_be_poweroff()     { epd_poweroff(); }
void epd_be_poweroff_all() { epd_poweroff(); }   // upstream has no `_all` variant

void epd_be_clear() {
    epd_clear();
}

void epd_be_clear_area_cycles(HostEpdRect area, int32_t cycles, int32_t cycle_time) {
    ::EpdRect a = { (int)area.x, (int)area.y, (int)area.width, (int)area.height };

    // Long clear cycles can approach the task-WDT threshold under stress
    // (6 cycles × ~50 ms baseline = ~300 ms, growing with cycle_time and
    // larger clear regions). Feed the watchdog defensively around the
    // blocking lib call. esp_task_wdt_status(NULL) is ESP_OK only when
    // the current task is subscribed to the TWDT — guard the reset so we
    // don't trip ESP_ERR_NOT_FOUND on non-subscribed tasks.
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }

    // Always do the actual cycle-clearing — display.cpp's caller asked for a
    // specific number of flashes (6 for full refresh, 1 for fast). epdiy's
    // higher-level epd_fullclear() ignores that count and only does a single
    // panel flash, which leaves visible ghosting on the Pro panel.
    epd_clear_area_cycles(a, (int)cycles, (int)cycle_time);

    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }

    // For a full-screen clear, sync BOTH hl_state framebuffers to all-white.
    // front_fb is the draw buffer; back_fb is epdiy's model of "what the
    // panel currently shows" and drives the MODE_GC16 differential
    // transitions in the next epd_hl_update_screen. If we only reset
    // front_fb (what epd_hl_set_all_white does), the next update computes
    // transitions from a stale back_fb (= last drawn content) even though
    // the panel itself is now white — and re-paints the old content as
    // ghost transitions. Memset'ing both keeps epdiy honest about panel
    // state without an extra panel refresh.
    if (a.x == 0 && a.y == 0 && a.width == epd_width() && a.height == epd_height()) {
        const int fb_size = epd_width() / 2 * epd_height();
        memset(hl_state.front_fb, 0xFF, fb_size);
        memset(hl_state.back_fb,  0xFF, fb_size);
    }
}

int epd_be_draw_grayscale_image(HostEpdRect area, uint8_t *data,
                                int mode, int temp_c) {
    ::EpdRect a = { (int)area.x, (int)area.y, (int)area.width, (int)area.height };
    epd_copy_to_framebuffer(a, data, epd_hl_get_framebuffer(&hl_state));

    // Resolve header-level sentinels to backend defaults. The header
    // intentionally does not expose EpdDrawMode (that lives in epdiy.h),
    // so callers signal "default" via EPD_BE_DEFAULT_MODE / -TEMP_C.
    const EpdDrawMode resolved_mode =
        (mode == EPD_BE_DEFAULT_MODE) ? MODE_GC16 : (EpdDrawMode)mode;
    const int resolved_temp =
        (temp_c == EPD_BE_DEFAULT_TEMP_C) ? DEFAULT_TEMPERATURE_C : temp_c;

    // Capture the EpdDrawError bitfield from the waveform driver. Silently
    // dropping it (the old void signature) meant that on FAILED_ALLOC /
    // MODE_NOT_FOUND / NO_PHASES_AVAILABLE the panel was not actually
    // refreshed, but epdiy's back_fb had already been advanced to the new
    // content — every subsequent partial diff was then computed against a
    // baseline that didn't match what the panel actually shows, accumulating
    // corruption until the next full GC16 clear.
    enum EpdDrawError err;
    const int fw = epd_width();
    const int fh = epd_height();
    if (a.x == 0 && a.y == 0 && a.width == fw && a.height == fh) {
        err = epd_hl_update_screen(&hl_state, resolved_mode, resolved_temp);
    } else {
        err = epd_hl_update_area(&hl_state, resolved_mode, resolved_temp, a);
    }

    if (err != EPD_DRAW_SUCCESS) {
        Serial.printf("[EPD] draw error 0x%x area=(%d,%d,%d,%d) mode=0x%x\n",
                      (unsigned)err,
                      (int)a.x, (int)a.y, (int)a.width, (int)a.height,
                      (unsigned)resolved_mode);
    }
    return (int)err;
}

HostEpdRect epd_be_full_screen() {
    ::EpdRect a = epd_full_screen();
    return HostEpdRect{ a.x, a.y, a.width, a.height };
}

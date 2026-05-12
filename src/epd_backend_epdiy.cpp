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
#include <string.h>   // memset for direct hl_state buffer reset

#define EpdRect HostEpdRect
#include "epd_backend.h"
#undef EpdRect

extern "C" {
#include "epdiy.h"
#include "epd_highlevel.h"
}

// Confirm ABI compatibility between the two rect types at compile time.
static_assert(sizeof(HostEpdRect) == sizeof(::EpdRect),
              "HostEpdRect / epdiy EpdRect size mismatch");

static EpdiyHighlevelState hl_state;

// Pro hardware VCOM (millivolts, absolute value; the chip applies negative).
// LilyGo's reference example uses 1560 mV (= -1.56 V) but that left text and
// dark UI backgrounds visibly washed out on our panel. Bumped to 2400 mV
// (= -2.40 V) for stronger contrast. Safe range for ED047TC1 is roughly
// 1500–2500 mV; tune within that window if the panel charge of a specific
// board behaves differently.
static constexpr uint16_t PRO_VCOM_MV = 2400;

// Ambient temperature passthrough constant. The Pro board's TPS65185 reports
// real temperature via epd_ambient_temperature(); for simplicity (and because
// our display.cpp does not currently pipe temperature data through the
// backend), we pass a sensible default per upstream examples.
static constexpr int DEFAULT_TEMPERATURE_C = 25;

void epd_be_init() {
    epd_init(&epd_board_v7, &ED047TC1, EPD_OPTIONS_DEFAULT);
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

    // Always do the actual cycle-clearing — display.cpp's caller asked for a
    // specific number of flashes (6 for full refresh, 1 for fast). epdiy's
    // higher-level epd_fullclear() ignores that count and only does a single
    // panel flash, which leaves visible ghosting on the Pro panel.
    epd_clear_area_cycles(a, (int)cycles, (int)cycle_time);

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

void epd_be_draw_grayscale_image(HostEpdRect area, uint8_t *data) {
    ::EpdRect a = { (int)area.x, (int)area.y, (int)area.width, (int)area.height };
    epd_copy_to_framebuffer(a, data, epd_hl_get_framebuffer(&hl_state));

    const int fw = epd_width();
    const int fh = epd_height();
    if (a.x == 0 && a.y == 0 && a.width == fw && a.height == fh) {
        epd_hl_update_screen(&hl_state, MODE_GC16, DEFAULT_TEMPERATURE_C);
    } else {
        epd_hl_update_area(&hl_state, MODE_GC16, DEFAULT_TEMPERATURE_C, a);
    }
}

HostEpdRect epd_be_full_screen() {
    ::EpdRect a = epd_full_screen();
    return HostEpdRect{ a.x, a.y, a.width, a.height };
}

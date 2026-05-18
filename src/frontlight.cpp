#include "frontlight.h"
#include "config.h"
#include "settings.h"

#ifndef BL_PIN
#define BL_PIN 11   // BOARD_BL_EN on T5S3 4.7 PRO
#endif

// 20 kHz keeps PWM well above the EPD line-rate (~50 µs lines on epdiy v7)
// and removes a periodic luminance coupling artifact that was visible at
// the previous 5 kHz carrier frequency. Still inaudible and well within
// LEDC's resolution range (240 MHz / 256 = 937 kHz max for 8-bit).
static const int BL_LEDC_FREQ    = 20000;
static const int BL_LEDC_BITS    = 8;
static const int BL_LEDC_MAX     = (1 << BL_LEDC_BITS) - 1;

static bool _inited = false;

void frontlight_init() {
    if (_inited) return;
    ledcAttach(BL_PIN, BL_LEDC_FREQ, BL_LEDC_BITS);
    ledcWrite(BL_PIN, 0);
    _inited = true;
}

void frontlight_apply(bool enabled, uint8_t brightnessPercent) {
    if (!_inited) frontlight_init();
    if (brightnessPercent > 100) brightnessPercent = 100;
    uint32_t duty = enabled ? (uint32_t)brightnessPercent * BL_LEDC_MAX / 100 : 0;
    ledcWrite(BL_PIN, duty);
}

void frontlight_apply_from_settings() {
    const Settings& s = settings_get();
    frontlight_apply(s.frontlightEnabled, s.frontlightBrightness);
}

void frontlight_off() {
    if (!_inited) return;
    ledcWrite(BL_PIN, 0);
}

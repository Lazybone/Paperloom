#pragma once

#include <Arduino.h>

// Frontlight control for LilyGo T5S3 4.7" e-paper PRO (BOARD_BL_EN = GPIO 11).
// No-op on non-PRO targets.
void frontlight_init();

// brightness: 0..100 (percent). enabled=false forces off regardless of brightness.
void frontlight_apply(bool enabled, uint8_t brightnessPercent);

// Convenience: read current settings and apply.
void frontlight_apply_from_settings();

// Quick off (sleep entry). Does not touch settings.
void frontlight_off();

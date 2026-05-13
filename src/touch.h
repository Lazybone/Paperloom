#pragma once

#include <Arduino.h>

struct TouchPoint {
    int16_t x;
    int16_t y;
};

bool touch_init();
bool touch_read(TouchPoint &pt);

// Put GT911 into low-power sleep (~1mA savings). Wake only via hardware
// reset on next boot — do not call before light sleep, only deep sleep.
void touch_sleep();

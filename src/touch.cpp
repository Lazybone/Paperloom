#include "touch.h"
#include "config.h"
#include <Wire.h>
#include <TouchDrvGT911.hpp>
#include <driver/gpio.h>

// Pro touch via Arduino Wire. Wire is initialized in main.cpp::setup()
// BEFORE display_init(), so Wire owns the I²C-0 driver and epdiy (patched
// by tools/patch_epdiy.py) reuses it instead of trying to install its own.
//
// We hand the real TOUCH_RST / TOUCH_INT_PIN to SensorLib — same wiring as
// the LilyGo T5S3-PRO factory example (examples/factory/main/main.cpp:331).
// SensorLib's GT911 init then performs the documented hardware reset:
//   RST LOW + INT LOW (selects address 0x5D) → 120 µs → RST HIGH → 8 ms.
//
// Earlier this file passed -1/-1 to setPins to avoid an observed
// "next display update crashes" symptom. Cause turned out to be a stale
// gpio_hold from a prior deep-sleep boot — handled below by calling
// gpio_hold_dis() on both pins before SensorLib touches them.

static TouchDrvGT911 _touch;
static bool _initialized = false;

bool touch_init() {
    // Release any leftover deep-sleep hold on the GT911 control pins.
    // Hold-state survives the soft reset on wake; if it's still active the
    // SensorLib reset sequence below silently does nothing and the chip
    // never comes out of sleep. Boards that never armed a hold (cold boot)
    // just ignore this — gpio_hold_dis on a free pin is a no-op.
    gpio_hold_dis((gpio_num_t)TOUCH_RST);
    gpio_hold_dis((gpio_num_t)TOUCH_INT_PIN);

    _touch.setPins(TOUCH_RST, TOUCH_INT_PIN);

    if (!_touch.begin(Wire, TOUCH_ADDR, TOUCH_SDA, TOUCH_SCL)) {
        Serial.printf("GT911 begin() failed at 0x%02X\n", TOUCH_ADDR);
        return false;
    }

    _touch.setMaxCoordinates(PHYS_WIDTH, PHYS_HEIGHT);
    _touch.setSwapXY(true);
    _touch.setMirrorXY(false, true);

    Serial.printf("GT911 initialized at 0x%02X (Pro, shared Wire on I²C-0)\n", TOUCH_ADDR);
    _initialized = true;
    return true;
}

void touch_sleep() {
    if (!_initialized) return;
    // GT911 command 0x05 = enter low-power sleep. Chip stops scanning the
    // capacitive matrix; idle current drops from ~1-2 mA to <100 µA.
    // Recovery requires hardware reset, which touch_init() performs via
    // SensorLib's RST/INT toggle on the next boot.
    _touch.sleep();
}

bool touch_read(TouchPoint &pt) {
    if (!_initialized) return false;

    // Read point array directly instead of calling isPressed() first.
    // isPressed() polls the status register, which on the GT911 also
    // clears the data-ready buffer — a subsequent getPoint() then finds
    // an empty buffer and reports zero touches. Single getPoint() returns
    // count and coords in one transaction.
    int16_t x[1] = {0};
    int16_t y[1] = {0};
    uint8_t count = _touch.getPoint(x, y, 1);
    if (count == 0) return false;

    pt.x = (PORTRAIT_W - 1) - y[0];
    pt.y = x[0];
    return true;
}

#include "battery.h"
#include "config.h"
#include <Wire.h>

// Wire is initialized very early in main.cpp::setup() (before display_init)
// so that our Wire owns the I²C-0 driver and epdiy reuses it. We just read
// registers normally — no Wire.begin() here.

static bool _bq_ok = false;

static bool bq_read_u16(uint8_t reg, uint16_t &out) {
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(true) != 0) return false;
    if (Wire.requestFrom(BQ27220_ADDR, (uint8_t)2) != 2) return false;
    uint8_t lo = Wire.read();
    uint8_t hi = Wire.read();
    out = (uint16_t)(lo | (hi << 8));
    return true;
}

void battery_init() {
    uint16_t voltage_mV = 0;
    _bq_ok = bq_read_u16(0x08 /* Voltage */, voltage_mV);
    Serial.printf("[BAT] BQ27220 init %s (V=%u mV)\n",
                  _bq_ok ? "OK" : "FAIL", voltage_mV);
}

float battery_voltage() {
    if (!_bq_ok) return 0.0f;
    uint16_t mV = 0;
    if (!bq_read_u16(0x08 /* Voltage */, mV)) return 0.0f;
    return mV / 1000.0f;
}

int battery_percent() {
    if (!_bq_ok) return 0;
    uint16_t soc = 0;
    if (!bq_read_u16(0x2C /* StateOfCharge */, soc)) return 0;
    if (soc > 100) soc = 100;
    return (int)soc;
}

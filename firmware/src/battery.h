#pragma once

#include <stdint.h>

struct BatteryState {
    uint32_t millivolts;  // smoothed cell voltage
    int percent;          // estimated from a LiPo discharge curve
    bool external_power;  // USB host attached, or cell held at charge voltage
    bool low;             // latched below BATTERY_LOW_PERCENT, cleared above BATTERY_OK_PERCENT
    bool known;           // false on external power: the ADC sees the system rail, which hides the cell
};

#define BATTERY_LOW_PERCENT 15
#define BATTERY_OK_PERCENT 20

// Samples the battery. Returns true when the state changed enough to be worth showing or reporting.
bool battery_update(BatteryState *state);

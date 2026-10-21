// Battery estimate from the system rail voltage. There is no fuel gauge or charge-status pin on this
// board, and on USB power the rail sits above any cell voltage, so the level is only known on battery.
#include "battery.h"

#include <Arduino.h>

#include "board.h"
#include "driver/usb_serial_jtag.h"

#define SAMPLES 64
#define CHARGE_VOLTAGE_MV 4150  // at or above this the cell is on (or just off) the charger
#define SYSTEM_RAIL_MV 4400     // no LiPo gets here; the rail is being fed from USB
#define RESYNC_MV 300           // a jump this big is a power source change, not noise, so skip smoothing

// Resting LiPo voltage -> state of charge.
static const struct {
    uint16_t mv;
    uint8_t percent;
} kCurve[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70},
    {3910, 65}, {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35},
    {3770, 30}, {3750, 25}, {3730, 20}, {3710, 15}, {3690, 10}, {3610, 5}, {3270, 0},
};

static int percent_for(uint32_t mv)
{
    const int n = sizeof(kCurve) / sizeof(kCurve[0]);
    if (mv >= kCurve[0].mv) return 100;
    if (mv <= kCurve[n - 1].mv) return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= kCurve[i].mv) {
            const int span_mv = kCurve[i - 1].mv - kCurve[i].mv;
            const int span_pct = kCurve[i - 1].percent - kCurve[i].percent;
            return kCurve[i].percent + (int)(mv - kCurve[i].mv) * span_pct / span_mv;
        }
    }
    return 0;
}

bool battery_update(BatteryState *state)
{
    uint32_t sum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        sum += analogReadMilliVolts(PIN_BATTERY_ADC);
    }
    const uint32_t mv = sum / SAMPLES * 2;  // undo the 1:2 divider

    const BatteryState before = *state;
    const bool resync = !state->millivolts || abs((int)mv - (int)state->millivolts) > RESYNC_MV;
    state->millivolts = resync ? mv : (state->millivolts * 7 + mv) / 8;
    state->known = state->millivolts < SYSTEM_RAIL_MV;
    state->external_power = usb_serial_jtag_is_connected() || state->millivolts >= CHARGE_VOLTAGE_MV;
    state->percent = percent_for(state->millivolts);
    // While discharging, never show the level going back up: that is ADC noise or load changes, not charge.
    const bool discharging = state->known && !state->external_power;
    const bool was_discharging = before.known && !before.external_power;
    if (discharging && was_discharging && state->percent > before.percent) {
        state->percent = before.percent;
    }
    if (!state->known || state->external_power || state->percent >= BATTERY_OK_PERCENT) {
        state->low = false;
    } else if (state->percent <= BATTERY_LOW_PERCENT) {
        state->low = true;
    }

    return before.percent != state->percent || before.external_power != state->external_power ||
           before.low != state->low || before.known != state->known;
}

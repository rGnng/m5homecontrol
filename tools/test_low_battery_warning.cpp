#include "low_battery_warning.h"
#include <cassert>
#include <iostream>

int main() {
    for (int percent : {-10, -1, 0, 24, 25, 26, 50, 100}) {
        assert(lowBatteryWarningEligible(true, false, percent) == (percent >= 0 && percent < 25));
        assert(!lowBatteryWarningEligible(false, false, percent));
        assert(!lowBatteryWarningEligible(true, true, percent));
    }
    LowBatteryWarning warning;
    assert(!warning.tick(false, true, 0));
    assert(!warning.tick(true, false, 100));  // Busy LEDs defer the first warning.
    assert(warning.tick(true, true, 200));
    assert(!warning.tick(true, true, 60199));
    assert(warning.tick(true, true, 60200));
    assert(!warning.tick(true, false, 120200));
    assert(warning.tick(true, true, 120300));  // Busy feedback does not consume a due warning.
    assert(!warning.tick(false, true, 120400));  // Docked, disabled, recovered or unknown.
    assert(!warning.tick(true, true, 120500)); // A transient fault cannot accelerate the cadence.
    assert(warning.tick(true, true, 180300));
    LowBatteryWarning rollover;
    assert(rollover.tick(true, true, 0xfffffff0u));
    assert(!rollover.tick(true, true, 59983));
    assert(rollover.tick(true, true, 59984));
    int pulses = 0;
    uint8_t previous = 0;
    for (uint32_t elapsed = 0; elapsed <= 400; ++elapsed) {
        auto red = lowBatteryLedRed(elapsed);
        assert(red == 0 || red == 38);
        if (red && !previous) ++pulses;
        if (elapsed < 80 || (elapsed >= 160 && elapsed < 240)) assert(red == 38);
        else assert(red == 0);
        previous = red;
    }
    assert(pulses == 2);
    std::cout << "PASS: strict low threshold, dock/unknown/disabled suppression, minute cadence, busy deferral, rollover and two red pulses\n";
}

#include "power_status.h"

#include <cassert>
#include <cstdio>

int main() {
    // Actual docked sample: a full coarse gauge, inactive Core charger, and
    // no evidence about the separate dock charger.
    auto dock = decodeIp5306Power(true, 23, true, 0, true, 0);
    assert(dock.batteryPercent == 100 && dock.coreInput == 0);
    // An IP5306 battery percentage does not provide a voltage measurement.
    assert(dock.batteryMillivolts == -1 && dock.inputMillivolts == -1);
    assert(dock.coreCharging == ChargeState::Inactive);
    assert(dock.charging == ChargeState::Unknown);

    // Actual USB sample, then a USB charge-complete sample.
    auto usb = decodeIp5306Power(true, 153, true, 112, true, 0);
    assert(usb.coreInput == 1 && usb.charging == ChargeState::Charging);
    auto full = decodeIp5306Power(true, 0x08, true, 0x08, true, 0);
    assert(full.charging == ChargeState::Full);

    // Failed reads must not turn zero-initialized bytes into 100% or OFF.
    auto missing = decodeIp5306Power(false, 0, false, 0, false, 0);
    assert(missing.batteryPercent == -1 && missing.coreInput == -1);
    assert(missing.coreCharging == ChargeState::Unknown);
    assert(missing.charging == ChargeState::Unknown);
    auto partial = decodeIp5306Power(true, 0x08, false, 0, true, 0xc0);
    assert(partial.batteryPercent == 50 && partial.coreInput == 1);
    assert(partial.charging == ChargeState::Unknown);

    // A stale full flag without input cannot establish a full/charging state.
    auto noInput = decodeIp5306Power(true, 0, true, 0x08, true, 0x80);
    assert(noInput.batteryPercent == 75 && noInput.charging == ChargeState::Unknown);
    auto noInputRead = decodeIp5306Power(false, 0, true, 0x08, true, 0);
    assert(noInputRead.charging == ChargeState::Unknown);

    const uint8_t codes[] = {0x00, 0x80, 0xc0, 0xe0, 0xf0};
    const int percentages[] = {100, 75, 50, 25, 0};
    for (unsigned i = 0; i < 5; ++i) {
        auto sample = decodeIp5306Power(true, 0, true, 0, true, codes[i]);
        assert(sample.batteryPercent == percentages[i]);
    }
    auto invalid = decodeIp5306Power(true, 0, true, 0, true, 0x70);
    assert(invalid.batteryPercent == -1);
    std::puts("PASS: dock, USB, full, absent/partial telemetry, stale flags, gauge decoding");
}

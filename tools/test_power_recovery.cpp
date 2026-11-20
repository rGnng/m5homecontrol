#include <cassert>
#include <cstdio>

#include "M5Unified.h"
#include "power_status.h"

uint32_t testMillis = 0;
Device M5;
Logger Serial;

int main() {
    auto status = readPowerStatus();
    assert(status.batteryPercent == -1 && !status.pmicOnline);
    unsigned reads = M5.Power.Ip5306.reads;
    testMillis = 100;
    readPowerStatus();
    assert(M5.Power.Ip5306.reads == reads);

    // A controller coming online after boot recovers without restarting the app.
    testMillis = 500;
    M5.Power.Ip5306.online = true;
    status = readPowerStatus();
    assert(status.batteryPercent == 100 && status.coreKeepOn == 1);
    assert(status.recoveryAttempts == 0);
    assert(status.coreBoostEnabled == 1 && status.coreChargeEnabled == 1);
    assert(status.coreBatteryHandoff == 1);
    // Reserved bits, the power-button setting, and low-battery cutoff survive.
    assert(M5.Power.Ip5306.control == 0xb3 && M5.Power.Ip5306.handoff == 0x9d);
    assert(M5.Power.Ip5306.writes == 2);

    // Missing data replaces the old value, never presenting cached 100% as live.
    testMillis = 1000;
    M5.Power.Ip5306.online = false;
    status = readPowerStatus();
    assert(status.batteryPercent == -1);
    testMillis = 1500;
    readPowerStatus();
    assert(M5.In_I2C.begins == 0);
    testMillis = 2000;
    status = readPowerStatus();
    assert(status.recoveryAttempts == 1 && M5.In_I2C.begins == 1);
    testMillis = 2500;
    readPowerStatus();
    assert(M5.In_I2C.begins == 1);
    testMillis = 11500;
    readPowerStatus();
    assert(M5.In_I2C.begins == 1);

    // A subsequent retry succeeds; counters and current state reflect it.
    M5.Power.Ip5306.wakeOnBegin = true;
    testMillis = 12000;
    status = readPowerStatus();
    assert(status.recoveryAttempts == 2 && status.pmicOnline);
    assert(status.batteryPercent == 100 && status.coreKeepOn == 1);
    assert(M5.Power.Ip5306.writes == 2);
    testMillis = 12500;
    M5.Power.Ip5306.input = 8;
    status = readPowerStatus();
    assert(status.charging == ChargeState::Charging);
    std::puts("PASS: delayed startup, keep-on/handoff, preserved bits, failed reads, bounded recovery, USB transition");
}

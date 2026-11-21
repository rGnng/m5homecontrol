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
    assert(!status.dockInferred);
    unsigned reads = M5.Power.Ip5306.reads;
    testMillis = 100;
    assert(!readPowerStatus().dockInferred);
    assert(M5.Power.Ip5306.reads == reads);

    // A controller coming online after boot recovers without restarting the app.
    testMillis = 500;
    M5.Power.Ip5306.online = true;
    status = readPowerStatus();
    assert(status.batteryPercent == 100 && status.coreKeepOn == 1);
    assert(!status.dockInferred);
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
    assert(!status.dockInferred);
    testMillis = 1500;
    assert(!readPowerStatus().dockInferred);
    assert(M5.In_I2C.begins == 0);
    testMillis = 2000;
    status = readPowerStatus();
    assert(status.recoveryAttempts == 1 && M5.In_I2C.begins == 1);
    assert(status.dockInferred && status.charging == ChargeState::Unknown);
    assert(status.coreCharging == ChargeState::Unknown);
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
    assert(!status.dockInferred);
    assert(status.batteryPercent == 100 && status.coreKeepOn == 1);
    assert(M5.Power.Ip5306.writes == 2);
    testMillis = 12500;
    M5.Power.Ip5306.input = 8;
    status = readPowerStatus();
    assert(status.charging == ChargeState::Charging);
    assert(!status.dockInferred);
    // Connected idle samples at 5s, explicit power reads bypass that cache.
    setPowerPollingIdle(true);
    assert(powerSampleIntervalMs() == 5000);
    reads = M5.Power.Ip5306.reads;
    testMillis = 17000;
    readPowerStatus();
    assert(M5.Power.Ip5306.reads == reads);
    testMillis = 17500;
    readPowerStatus();
    assert(M5.Power.Ip5306.reads > reads);
    reads = M5.Power.Ip5306.reads;
    testMillis = 17501;
    readPowerStatus(true);
    assert(M5.Power.Ip5306.reads > reads);
    // Missing PMIC recovery backs off to 60s while idle.
    M5.Power.Ip5306.online = false;
    M5.Power.Ip5306.wakeOnBegin = false;
    testMillis = 22501; readPowerStatus();
    testMillis = 27501; readPowerStatus();
    testMillis = 32501; readPowerStatus();
    assert(M5.In_I2C.begins == 2);
    testMillis = 72501; readPowerStatus();
    assert(M5.In_I2C.begins == 3);
    testMillis = 127501; readPowerStatus();
    assert(M5.In_I2C.begins == 3);
    testMillis = 132501; readPowerStatus();
    assert(M5.In_I2C.begins == 4);
    setPowerPollingIdle(false);
    assert(powerSampleIntervalMs() == 500);
    std::puts("PASS: delayed startup, keep-on/handoff, preserved bits, debounced dock hint, unknown charge status, recovery, USB transition");
}

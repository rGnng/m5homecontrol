#include "power_status.h"

#include <M5Unified.h>

static bool g_powerPollingIdle = false;
void setPowerPollingIdle(bool idle) { g_powerPollingIdle = idle; }
uint32_t powerSampleIntervalMs() { return g_powerPollingIdle ? 5000 : 500; }

PowerStatus readPowerStatus(bool force) {
    static PowerStatus status;
    static uint32_t sampledAt = 0;
    static bool sampled = false;
    uint32_t now = millis();
    if (!force && sampled && now - sampledAt < powerSampleIntervalMs()) return status;
    sampled = true;
    sampledAt = now;

    if (M5.getBoard() == m5::board_t::board_M5Stack) {
        static unsigned failures = 0;
        static uint32_t recoveryAttempts = 0;
        static uint32_t lastRecovery = 0;
        // The keypad shares this bus; use its conservative 100 kHz clock.
        M5.Power.Ip5306.setClock(100000);
        uint8_t input = 0, full = 0, level = 0;
        bool inputOk = false, fullOk = false, levelOk = false;
        auto sample = [&]() {
            inputOk = M5.Power.Ip5306.readRegister(0x70, &input, 1);
            fullOk = M5.Power.Ip5306.readRegister(0x71, &full, 1);
            levelOk = M5.Power.Ip5306.readRegister(0x78, &level, 1);
        };
        sample();
        bool online = inputOk || fullOk || levelOk;
        failures = online ? 0 : failures + 1;
        uint32_t recoveryInterval = g_powerPollingIdle ? 60000 : 10000;
        if (failures >= 3 && (!recoveryAttempts || now - lastRecovery >= recoveryInterval)) {
            ++recoveryAttempts;
            lastRecovery = now;
            // Retry a late/stuck bus without restarting the ESP32 or applying
            // Power.begin()'s charger-current/voltage defaults again.
            M5.In_I2C.begin();
            M5.Power.Ip5306.begin();
            sample();
            if (inputOk || fullOk || levelOk) failures = 0;
            Serial.printf("[power] recovery %lu: %s\n", (unsigned long)recoveryAttempts,
                          failures ? "PMIC unavailable" : "PMIC responding");
        }
        status = decodeIp5306Power(inputOk, input, fullOk, full, levelOk, level);
        status.recoveryAttempts = recoveryAttempts;
        uint8_t control = 0;
        if (M5.Power.Ip5306.readRegister(0x00, &control, 1)) {
            status.pmicOnline = true;
            // Dock power can bypass the Core boost output. Keep that output
            // ready for battery handoff despite its light load. This does not
            // guarantee that PMIC telemetry stays available while docked.
            // Runtime requires boost, charge enable, and boost keep-on. Only
            // those bits change; voltage/current and reserved bits survive.
            if ((control & 0x32) != 0x32) {
                M5.Power.Ip5306.writeRegister8(0x00, control | 0x32);
            }
            if (M5.Power.Ip5306.readRegister(0x00, &control, 1)) {
                status.coreKeepOn = (control & 0x02) ? 1 : 0;
                status.coreBoostEnabled = (control & 0x20) ? 1 : 0;
                status.coreChargeEnabled = (control & 0x10) ? 1 : 0;
            }
        }
        uint8_t handoff = 0;
        if (M5.Power.Ip5306.readRegister(0x01, &handoff, 1)) {
            // Restore boost automatically after the Core's input is removed.
            // Retain the low-battery cutoff and power-button behavior.
            if (!(handoff & 0x04)) {
                M5.Power.Ip5306.writeRegister8(0x01, handoff | 0x04);
            }
            if (M5.Power.Ip5306.readRegister(0x01, &handoff, 1)) {
                status.coreBatteryHandoff = (handoff & 0x04) ? 1 : 0;
            }
        }
        // User-selected dock heuristic: do not treat one failed I2C sample as
        // docking. This is an external-power hint, not charger telemetry.
        static uint8_t unavailableSamples = 0;
        if (status.pmicOnline) unavailableSamples = 0;
        else if (unavailableSamples < 3) ++unavailableSamples;
        status.dockInferred = unavailableSamples >= 3;
    } else {
        status = PowerStatus();
        status.batteryPercent = M5.Power.getBatteryLevel();
        int batteryVoltage = M5.Power.getBatteryVoltage();
        if (batteryVoltage > 0) status.batteryMillivolts = batteryVoltage;
        int vbus = M5.Power.getVBUSVoltage();
        if (vbus >= 0) status.inputMillivolts = vbus;
        if (vbus >= 0) status.coreInput = vbus > 4000 ? 1 : 0;
        auto charging = M5.Power.isCharging();
        status.pmicOnline = status.batteryPercent >= 0 ||
                            charging != m5::Power_Class::charge_unknown;
        if (charging == m5::Power_Class::is_charging) {
            status.coreCharging = ChargeState::Charging;
            status.charging = ChargeState::Charging;
            status.coreInput = 1;
        } else if (charging == m5::Power_Class::is_discharging) {
            status.coreCharging = ChargeState::Inactive;
        }
    }
    return status;
}

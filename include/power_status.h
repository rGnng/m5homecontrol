#pragma once

#include <cstdint>

enum class ChargeState : uint8_t { Unknown, Inactive, Charging, Full };

struct PowerStatus {
    int batteryPercent = -1;
    int batteryMillivolts = -1;
    int inputMillivolts = -1;
    int coreInput = -1;  // -1 unavailable, 0 absent, 1 present
    ChargeState coreCharging = ChargeState::Unknown;
    ChargeState charging = ChargeState::Unknown;
    bool pmicOnline = false;
    int coreKeepOn = -1;
    int coreBoostEnabled = -1;
    int coreChargeEnabled = -1;
    int coreBatteryHandoff = -1;
    uint32_t recoveryAttempts = 0;
};

inline const char* chargeStateName(ChargeState state) {
    switch (state) {
        case ChargeState::Inactive: return "inactive";
        case ChargeState::Charging: return "charging";
        case ChargeState::Full: return "full";
        default: return "unknown";
    }
}

// The Core's IP5306 cannot observe the Faces Bottom3 TP4057. Its inactive
// charger does not establish whether the battery is charging through the dock.
inline PowerStatus decodeIp5306Power(bool inputOk, uint8_t input,
                                    bool fullOk, uint8_t full,
                                    bool levelOk, uint8_t level) {
    PowerStatus status;
    status.pmicOnline = inputOk || fullOk || levelOk;
    if (levelOk) {
        switch (level >> 4) {
            case 0x0: status.batteryPercent = 100; break;
            case 0x8: status.batteryPercent = 75; break;
            case 0xc: status.batteryPercent = 50; break;
            case 0xe: status.batteryPercent = 25; break;
            case 0xf: status.batteryPercent = 0; break;
            default: break;
        }
    }
    if (inputOk) {
        status.coreInput = (input & 0x08) ? 1 : 0;
        if (!status.coreInput) {
            status.coreCharging = ChargeState::Inactive;
        } else if (fullOk) {
            status.coreCharging = (full & 0x08) ? ChargeState::Full : ChargeState::Charging;
            status.charging = status.coreCharging;
        }
    }
    return status;
}

// One shared sample for the display and diagnostics; refreshes every 500 ms.
PowerStatus readPowerStatus();

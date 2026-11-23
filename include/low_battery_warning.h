#pragma once

#include <cstdint>

static constexpr uint32_t LOW_BATTERY_WARNING_INTERVAL_MS = 60000;
static constexpr uint32_t LOW_BATTERY_LED_FLASH_MS = 80;
static constexpr uint32_t LOW_BATTERY_LED_DURATION_MS = 3 * LOW_BATTERY_LED_FLASH_MS;
static constexpr uint8_t LOW_BATTERY_LED_RED = 38;  // 15% of 255, independent of LCD brightness.

inline bool lowBatteryWarningEligible(bool enabled, bool docked, int percent) {
    return enabled && !docked && percent >= 0 && percent < 25;
}

inline uint8_t lowBatteryLedRed(uint32_t elapsed) {
    return elapsed < LOW_BATTERY_LED_DURATION_MS &&
           (elapsed / LOW_BATTERY_LED_FLASH_MS) % 2 == 0 ? LOW_BATTERY_LED_RED : 0;
}

class LowBatteryWarning {
public:
    bool tick(bool eligible, bool ledReady, uint32_t now) {
        // Keep the last warning time across unknown samples and dock changes,
        // so an intermittent reading cannot produce warnings faster than once a minute.
        if (!eligible || !ledReady ||
            (warned_ && uint32_t(now - lastWarning_) < LOW_BATTERY_WARNING_INTERVAL_MS))
            return false;
        warned_ = true;
        lastWarning_ = now;
        return true;
    }

private:
    bool warned_ = false;
    uint32_t lastWarning_ = 0;
};

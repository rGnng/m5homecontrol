#pragma once

#include <cstdint>

// One battery-only sequence, independent of the day/night display schedule.
class SleepSequence {
public:
    void reset() { offActive_ = lightStarted_ = false; }
    void screenOff(uint32_t startedAt) {
        if (!offActive_ || offAt_ != startedAt) {
            offAt_ = startedAt;
            offActive_ = true;
            lightStarted_ = false;
        }
    }
    bool lightDue(uint32_t now, uint16_t minutes) const {
        return offActive_ && minutes && uint32_t(now - offAt_) >= uint32_t(minutes) * 60000;
    }
    bool lightStarted() const { return lightStarted_; }
    void startLight(uint32_t now) {
        if (!lightStarted_) { lightAt_ = now; lightStarted_ = true; }
    }
    bool deepDue(uint32_t now, uint16_t minutes) const {
        return lightStarted_ && minutes && uint32_t(now - lightAt_) >= uint32_t(minutes) * 60000;
    }
    uint32_t pollInterval(uint32_t now, uint32_t healthMs, uint16_t deepMinutes) const {
        if (!lightStarted_ || !deepMinutes) return healthMs;
        uint32_t elapsed = now - lightAt_, deadline = uint32_t(deepMinutes) * 60000;
        if (elapsed >= deadline) return healthMs; // Faulty keypad: keep light-sleep fallback.
        uint32_t remaining = deadline - elapsed;
        return remaining < healthMs ? remaining : healthMs;
    }

private:
    bool offActive_ = false, lightStarted_ = false;
    uint32_t offAt_ = 0, lightAt_ = 0;
};

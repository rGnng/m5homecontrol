#pragma once

#include <cstdint>

// Active-low contact test. The initial level establishes a baseline; only a
// subsequent stable open-to-grounded transition reports a docking event.
class DockInput {
public:
    static constexpr uint32_t DEBOUNCE_MS = 50;

    void begin(bool grounded, uint32_t now) {
        stable_ = candidate_ = grounded;
        candidateSince_ = now;
        dockingEvents_ = undockingEvents_ = 0;
    }

    bool update(bool grounded, uint32_t now) {
        if (grounded != candidate_) {
            candidate_ = grounded;
            candidateSince_ = now;
        }
        if (candidate_ == stable_ || uint32_t(now-candidateSince_) < DEBOUNCE_MS) return false;
        stable_ = candidate_;
        if (stable_) ++dockingEvents_;
        else ++undockingEvents_;
        return stable_;
    }

    bool docked() const { return stable_; }
    bool pending() const { return candidate_ != stable_; }
    uint32_t dockingEvents() const { return dockingEvents_; }
    uint32_t undockingEvents() const { return undockingEvents_; }

private:
    bool stable_ = false, candidate_ = false;
    uint32_t candidateSince_ = 0, dockingEvents_ = 0, undockingEvents_ = 0;
};

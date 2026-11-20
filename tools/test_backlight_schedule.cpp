#include "backlight_schedule.h"

#include <cassert>
#include <cstdio>

int main() {
    auto night = [](uint32_t idle) { return planBacklight(idle, 5, 1, 600, 120); };
    assert(night(0).phase == BacklightPhase::Normal && night(0).brightness == 5);
    assert(night(599999).phase == BacklightPhase::Normal);
    assert(night(600000).phase == BacklightPhase::Dim && night(600000).brightness == 1);
    assert(night(719999).phase == BacklightPhase::Dim);
    assert(night(720000).phase == BacklightPhase::Off && night(720000).brightness == 0);
    assert(night(3600000).phase == BacklightPhase::Off);
    // Resetting idle time wakes the display, with another complete dim interval.
    assert(night(0).phase == BacklightPhase::Normal);
    assert(night(600000).phase == BacklightPhase::Dim);
    // Day defaults, disabled timeouts, and legacy direct-to-off schedules.
    assert(planBacklight(3600000, 5, 1, 0, 0).phase == BacklightPhase::Normal);
    assert(planBacklight(119999, 5, 1, 0, 120).phase == BacklightPhase::Normal);
    assert(planBacklight(120000, 5, 1, 0, 120).phase == BacklightPhase::Off);
    assert(planBacklight(3600000, 5, 1, 600, 0).phase == BacklightPhase::Dim);
    assert(planBacklight(0, 0, 1, 600, 120).phase == BacklightPhase::Off);
    assert(planBacklight(600000, 2, 10, 600, 120).brightness == 2);
    // Unsigned elapsed time remains correct across millis() rollover.
    uint32_t lastInput = UINT32_MAX - 1000;
    uint32_t now = lastInput + 600000;
    assert(night(now - lastInput).phase == BacklightPhase::Dim);
    // Neither a schedule transition nor missing clock data may wake a screen.
    BacklightPlan normal = {BacklightPhase::Normal, 30};
    BacklightPlan dim = {BacklightPhase::Dim, 1};
    BacklightPlan off = {BacklightPhase::Off, 0};
    assert(limitAutomaticBacklight(normal, off).phase == BacklightPhase::Off);
    assert(limitAutomaticBacklight(dim, off).phase == BacklightPhase::Off);
    auto heldDim = limitAutomaticBacklight(normal, dim);
    assert(heldDim.phase == BacklightPhase::Dim && heldDim.brightness == 1);
    assert(limitAutomaticBacklight({BacklightPhase::Dim, 5}, dim).brightness == 1);
    assert(limitAutomaticBacklight({BacklightPhase::Dim, 1}, {BacklightPhase::Dim, 2}).brightness == 1);
    assert(limitAutomaticBacklight(off, dim).phase == BacklightPhase::Off);
    std::puts("PASS: normal/dim/off boundaries, wake, disabled timers, brightness cap, rollover, no automatic brightening");
}

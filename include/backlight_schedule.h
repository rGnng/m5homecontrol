#pragma once

#include <cstdint>

enum class BacklightPhase : uint8_t { Normal, Dim, Off };

struct BacklightPlan {
    BacklightPhase phase;
    uint8_t brightness;
};

// Off time counts from dimming, or from last input if dimming is disabled.
inline BacklightPlan planBacklight(uint32_t idleMillis, uint8_t brightness,
                                   uint8_t dimBrightness, uint16_t dimSeconds,
                                   uint16_t offSeconds) {
    if (brightness == 0) return {BacklightPhase::Off, 0};
    uint32_t dimAt = (uint32_t)dimSeconds * 1000;
    uint32_t offAt = dimAt + (uint32_t)offSeconds * 1000;
    if (offSeconds && idleMillis >= offAt) return {BacklightPhase::Off, 0};
    if (dimSeconds && idleMillis >= dimAt) {
        return {BacklightPhase::Dim, dimBrightness < brightness ? dimBrightness : brightness};
    }
    return {BacklightPhase::Normal, brightness};
}

// Background scheduling may darken the display. Callers bypass this guard for
// explicit input and the docked morning wake; clock availability alone cannot wake it.
inline BacklightPlan limitAutomaticBacklight(BacklightPlan next, BacklightPlan previous) {
    if (previous.phase == BacklightPhase::Off) return {BacklightPhase::Off, 0};
    if (previous.phase == BacklightPhase::Dim && next.phase != BacklightPhase::Off) {
        return {BacklightPhase::Dim,
                next.brightness < previous.brightness ? next.brightness : previous.brightness};
    }
    return next;
}

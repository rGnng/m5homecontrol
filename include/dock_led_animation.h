#pragma once
#include <cstdint>

static constexpr uint32_t DOCK_LED_DURATION_MS = 350;
static constexpr uint8_t DOCK_LED_BRIGHTNESS_PERCENT = 15;
static constexpr uint8_t DOCK_LED_BRIGHTNESS = (255 * DOCK_LED_BRIGHTNESS_PERCENT + 50) / 100;
static constexpr uint32_t DOCK_LED_FADE_MS = DOCK_LED_DURATION_MS / 5;
struct DockLedColor { uint8_t red, green, blue; };

inline DockLedColor dockLedColor(uint32_t elapsed) {
    if (elapsed > DOCK_LED_DURATION_MS) elapsed = DOCK_LED_DURATION_MS;
    // Fade in and out over the first and last 20% of the animation.
    uint32_t fade = elapsed < DOCK_LED_FADE_MS ? elapsed : DOCK_LED_FADE_MS;
    if (DOCK_LED_DURATION_MS - elapsed < fade) fade = DOCK_LED_DURATION_MS - elapsed;
    uint8_t brightness = (DOCK_LED_BRIGHTNESS * fade + DOCK_LED_FADE_MS/2) / DOCK_LED_FADE_MS;
    constexpr uint32_t midpoint = DOCK_LED_DURATION_MS / 2;
    if (elapsed <= midpoint) {
        return {brightness,
                static_cast<uint8_t>((brightness * elapsed + midpoint/2) / midpoint), 0};
    }
    constexpr uint32_t remaining = DOCK_LED_DURATION_MS - midpoint;
    return {static_cast<uint8_t>((brightness * (DOCK_LED_DURATION_MS-elapsed)
                                 + remaining/2) / remaining),
            brightness, 0};
}

#include "dock_led_animation.h"
#include <algorithm>
#include <cassert>
#include <iostream>
int main() {
    static_assert(DOCK_LED_DURATION_MS == 350 && DOCK_LED_BRIGHTNESS_PERCENT == 15 && DOCK_LED_BRIGHTNESS == 38);
    auto start = dockLedColor(0), middle = dockLedColor(175), end = dockLedColor(350);
    assert(start.red == 0 && start.green == 0 && start.blue == 0);
    assert(middle.red == 38 && middle.green == 38 && middle.blue == 0);
    assert(end.red == 0 && end.green == 0 && end.blue == 0);
    uint8_t previousPeak = 0;
    for (uint32_t elapsed=0; elapsed<=400; ++elapsed) {
        auto color = dockLedColor(elapsed);
        auto peak = std::max(color.red, color.green);
        assert(peak <= 38 && color.blue == 0);
        if (elapsed <= 70) assert(peak >= previousPeak);
        else if (elapsed <= 280) assert(peak == 38);
        else assert(peak <= previousPeak);
        if (elapsed <= 175) assert(color.red >= color.green);
        else assert(color.green >= color.red);
        if (elapsed >= 350) assert(peak == 0);
        previousPeak = peak;
    }
    auto early = dockLedColor(10), late = dockLedColor(340);
    assert(early.red > 0 && early.red < 38 && early.green == 0);
    assert(late.green > 0 && late.green < 38 && late.red == 0);
    std::cout << "PASS: 350 ms red-yellow-green animation, 15% peak, fading in and out\n";
}

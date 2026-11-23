#include "deep_sleep.h"

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>

static constexpr uint32_t DEEP_SLEEP_MAGIC = 0x44535033;
struct DeepSleepRecord {
    uint32_t magic, entries, keypadWakes, timerWakes, errors;
    bool pending, night;
    int dockLevel;
    uint32_t dockWakes, undockWakes;
};
static RTC_DATA_ATTR DeepSleepRecord g_deepSleep = {};
static const char* g_deepLastWake = "none";
static DeepWake g_deepWake = DeepWake::None;

DeepWake deepSleepWakeCause() { return g_deepWake; }
int deepSleepDockLevel() { return g_deepSleep.dockLevel; }
bool deepSleepWasNight() { return g_deepSleep.night; }

bool deepSleepBoot() {
    if (g_deepSleep.magic != DEEP_SLEEP_MAGIC || esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        g_deepSleep = {DEEP_SLEEP_MAGIC, 0, 0, 0, 0, false, false, -1, 0, 0};
        g_deepLastWake = "none";
        g_deepWake = DeepWake::None;
        return false;
    }
    bool sleepWake = g_deepSleep.pending;
    if (sleepWake) {
        auto cause = esp_sleep_get_wakeup_cause();
        if (cause == ESP_SLEEP_WAKEUP_EXT0) {
            ++g_deepSleep.keypadWakes;
            g_deepLastWake = "keypad";
            g_deepWake = DeepWake::Keypad;
        } else if (cause == ESP_SLEEP_WAKEUP_EXT1 && g_deepSleep.dockLevel >= 0) {
            g_deepWake = DeepWake::DockChange;
            if (g_deepSleep.dockLevel == 1) {
                ++g_deepSleep.dockWakes;
                g_deepLastWake = "dock";
            } else {
                ++g_deepSleep.undockWakes;
                g_deepLastWake = "undock";
            }
        } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
            ++g_deepSleep.timerWakes;
            g_deepLastWake = "timer";
            g_deepWake = DeepWake::Other;
        } else {
            g_deepLastWake = "other";
            g_deepWake = DeepWake::Other;
        }
        g_deepSleep.pending = false;
        // EXT0 configures this pad as RTC IO, including when the timer wins.
        rtc_gpio_deinit(GPIO_NUM_35);
        if (g_deepSleep.dockLevel >= 0) rtc_gpio_deinit(GPIO_NUM_26);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    }
    return sleepWake && g_deepWake != DeepWake::DockChange;
}

bool enterDeepSleep(int dockLevel, bool night) {
    // Do not sleep with an unread key: it would immediately wake us again.
    if (gpio_get_level(GPIO_NUM_35) == 0) return false;
    if (dockLevel >= 0 && gpio_get_level(GPIO_NUM_26) != dockLevel) return false;
    // A previous light-sleep health timer must not wake scheduled deep sleep.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    bool ready = esp_sleep_enable_ext0_wakeup(GPIO_NUM_35, 0) == ESP_OK;
    if (ready && dockLevel >= 0) {
        ready = rtc_gpio_init(GPIO_NUM_26) == ESP_OK &&
                rtc_gpio_set_direction(GPIO_NUM_26, RTC_GPIO_MODE_INPUT_ONLY) == ESP_OK &&
                rtc_gpio_pulldown_dis(GPIO_NUM_26) == ESP_OK &&
                rtc_gpio_pullup_en(GPIO_NUM_26) == ESP_OK &&
                esp_sleep_enable_ext1_wakeup(1ULL << 26,
                    dockLevel == 0 ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ALL_LOW) == ESP_OK;
    }
    if (!ready) {
        ++g_deepSleep.errors;
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        if (dockLevel >= 0) {
            rtc_gpio_deinit(GPIO_NUM_26);
            gpio_set_direction(GPIO_NUM_26, GPIO_MODE_INPUT);
            gpio_pullup_en(GPIO_NUM_26);
        }
        return false;
    }
    ++g_deepSleep.entries;
    g_deepSleep.pending = true;
    g_deepSleep.dockLevel = dockLevel;
    g_deepSleep.night = night;
    Serial.flush();
    esp_deep_sleep_start();
    return false;
}

String deepSleepStatus() {
    return "deep_sleep_entries=" + String(g_deepSleep.entries) +
           " deep_sleep_keypad_wakes=" + String(g_deepSleep.keypadWakes) +
           " deep_sleep_timer_wakes=" + String(g_deepSleep.timerWakes) +
           " deep_sleep_errors=" + String(g_deepSleep.errors) +
           " deep_sleep_dock_wakes=" + String(g_deepSleep.dockWakes) +
           " deep_sleep_undock_wakes=" + String(g_deepSleep.undockWakes) +
           " deep_sleep_last_wake=" + String(g_deepLastWake);
}

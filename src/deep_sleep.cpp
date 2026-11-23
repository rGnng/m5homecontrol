#include "deep_sleep.h"

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>

static constexpr uint32_t DEEP_SLEEP_MAGIC = 0x44535032;
struct DeepSleepRecord {
    uint32_t magic, entries, keypadWakes, timerWakes, errors;
    bool pending;
};
static RTC_DATA_ATTR DeepSleepRecord g_deepSleep = {};
static const char* g_deepLastWake = "none";

bool deepSleepBoot() {
    if (g_deepSleep.magic != DEEP_SLEEP_MAGIC || esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        g_deepSleep = {DEEP_SLEEP_MAGIC, 0, 0, 0, 0, false};
        g_deepLastWake = "none";
        return false;
    }
    bool sleepWake = g_deepSleep.pending;
    if (sleepWake) {
        auto cause = esp_sleep_get_wakeup_cause();
        if (cause == ESP_SLEEP_WAKEUP_EXT0) {
            ++g_deepSleep.keypadWakes;
            g_deepLastWake = "keypad";
        } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
            ++g_deepSleep.timerWakes;
            g_deepLastWake = "timer";
        } else {
            g_deepLastWake = "other";
        }
        g_deepSleep.pending = false;
        // EXT0 configures this pad as RTC IO, including when the timer wins.
        rtc_gpio_deinit(GPIO_NUM_35);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    }
    return sleepWake;
}

bool enterDeepSleep() {
    // Do not sleep with an unread key: it would immediately wake us again.
    if (gpio_get_level(GPIO_NUM_35) == 0) return false;
    // A previous light-sleep health timer must not wake scheduled deep sleep.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    bool ready = esp_sleep_enable_ext0_wakeup(GPIO_NUM_35, 0) == ESP_OK;
    if (!ready) {
        ++g_deepSleep.errors;
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        return false;
    }
    ++g_deepSleep.entries;
    g_deepSleep.pending = true;
    Serial.flush();
    esp_deep_sleep_start();
    return false;
}

String deepSleepStatus() {
    return "deep_sleep_entries=" + String(g_deepSleep.entries) +
           " deep_sleep_keypad_wakes=" + String(g_deepSleep.keypadWakes) +
           " deep_sleep_timer_wakes=" + String(g_deepSleep.timerWakes) +
           " deep_sleep_errors=" + String(g_deepSleep.errors) +
           " deep_sleep_last_wake=" + String(g_deepLastWake);
}

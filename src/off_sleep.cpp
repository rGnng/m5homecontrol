#include "off_sleep.h"

#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_timer.h>

static uint32_t g_sleepEntries = 0, g_sleepErrors = 0;
static uint64_t g_sleptMs = 0;
static OffWake g_lastWake = OffWake::Timer;
static constexpr gpio_num_t WAKE_PINS[] = {GPIO_NUM_39, GPIO_NUM_38, GPIO_NUM_37, GPIO_NUM_35};

// Wakeup changes int_type to a level trigger. gpio_wakeup_disable() only
// clears wakeup_enable on ESP32; it does not restore a normal polled input.
// All callers detach the keypad ISR before entry and restore its edge trigger
// afterward. Core buttons and the dock input are polled while awake.
static void disarmWakePin(gpio_num_t pin) {
    gpio_intr_disable(pin);
    gpio_wakeup_disable(pin);
    gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
}

void resetOffSleepWakePins() {
    for (auto pin : WAKE_PINS) disarmWakePin(pin);
    disarmWakePin(GPIO_NUM_26);
}

OffWake sleepWhileOff(bool keypadWake, uint32_t timerMs, int dockLevel) {
    unsigned count = keypadWake ? 4 : 3;
    unsigned armed = 0;
    bool ready = true;
    bool dockArmed = false;
    for (unsigned i = 0; i < count; ++i) {
        if (gpio_get_level(WAKE_PINS[i]) == 0) return OffWake::Input;
    }
    if (dockLevel >= 0 && gpio_get_level(GPIO_NUM_26) != dockLevel) return OffWake::DockChange;
    for (; armed < count; ++armed) {
        gpio_intr_disable(WAKE_PINS[armed]);
        if (gpio_wakeup_enable(WAKE_PINS[armed], GPIO_INTR_LOW_LEVEL) != ESP_OK) {
            ready = false;
            break;
        }
    }
    if (ready && dockLevel >= 0) {
        gpio_intr_disable(GPIO_NUM_26);
        dockArmed = gpio_wakeup_enable(GPIO_NUM_26,
            dockLevel == 0 ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL) == ESP_OK;
        ready = dockArmed;
    }
    ready = ready && esp_sleep_enable_gpio_wakeup() == ESP_OK;
    ready = ready && esp_sleep_enable_timer_wakeup(uint64_t(timerMs) * 1000) == ESP_OK;
    esp_err_t result = ESP_FAIL;
    int64_t startedAt = esp_timer_get_time();
    if (ready) {
        // Flush UART before sleep so the IDF doesn't wait on pending output.
        Serial.flush();
        result = esp_light_sleep_start();
    }
    for (unsigned i = 0; i < armed; ++i) disarmWakePin(WAKE_PINS[i]);
    if (dockArmed) disarmWakePin(GPIO_NUM_26);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    if (result != ESP_OK) {
        ++g_sleepErrors;
        g_lastWake = OffWake::Error;
        // Keep polling inputs if setup/sleep fails, without a tight retry loop.
        delay(100);
        return g_lastWake;
    }
    ++g_sleepEntries;
    g_sleptMs += uint64_t(esp_timer_get_time() - startedAt) / 1000;
    g_lastWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO ? OffWake::Input : OffWake::Timer;
    if (dockLevel >= 0) {
        bool input = false;
        for (unsigned i=0; i<count; ++i) input |= gpio_get_level(WAKE_PINS[i]) == 0;
        if (!input && (g_lastWake == OffWake::Input || gpio_get_level(GPIO_NUM_26) != dockLevel))
            g_lastWake = OffWake::DockChange;
    }
    return g_lastWake;
}

String offSleepStatus() {
    return "light_sleep_entries=" + String(g_sleepEntries) +
           " light_sleep_ms=" + String(static_cast<unsigned long>(g_sleptMs)) +
           " light_sleep_errors=" + String(g_sleepErrors) +
           " light_sleep_last_wake=" + String(g_lastWake == OffWake::Input ? "input" :
                                              g_lastWake == OffWake::DockChange ? "dock" :
                                              g_lastWake == OffWake::Timer ? "timer" : "error");
}

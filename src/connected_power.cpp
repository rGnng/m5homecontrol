#include "connected_power.h"

#include <esp32-hal-cpu.h>
#include <esp_wifi.h>

static bool g_connectedIdle = false;
static unsigned g_powerWork = 0;
static bool g_cpuConfigured = false;
static wifi_ps_type_t g_savedWifiSleep = WIFI_PS_NONE;
static bool g_restoreWifiSleep = false;

static void initializeConnectedCpu() {
    if (g_cpuConfigured) return;
    g_cpuConfigured = true;
    // Set the clock once before Wi-Fi starts. Runtime frequency switching
    // failed the device's wake/HTTP test, so idle never changes the CPU clock.
    if (getCpuFrequencyMhz() > 160 && !setCpuFrequencyMhz(160)) {
        Serial.printf("[power] fixed 160 MHz unavailable; retaining boot frequency\n");
    }
}

void setConnectedPowerIdle(bool idle) {
    g_connectedIdle = idle;
    initializeConnectedCpu();
}

bool connectedPowerIdle() { return g_connectedIdle; }

void beginConnectedPowerWork() {
    ++g_powerWork;
    initializeConnectedCpu();
    if (g_powerWork == 1) {
        g_restoreWifiSleep = esp_wifi_get_ps(&g_savedWifiSleep) == ESP_OK &&
            g_savedWifiSleep != WIFI_PS_NONE && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
    }
}

void endConnectedPowerWork() {
    if (g_powerWork) --g_powerWork;
    if (!g_powerWork && g_restoreWifiSleep) {
        esp_wifi_set_ps(g_savedWifiSleep);
        g_restoreWifiSleep = false;
    }
}

String connectedPowerStatus() {
    wifi_ps_type_t mode = WIFI_PS_NONE;
    bool known = esp_wifi_get_ps(&mode) == ESP_OK;
    return "power_idle=" + String(g_connectedIdle ? 1 : 0) +
           " cpu_policy=fixed" +
           " cpu_mhz=" + String(getCpuFrequencyMhz()) +
           " wifi_sleep=" + String(!known ? "unavailable" : mode == WIFI_PS_MIN_MODEM
                                      ? "minimum" : mode == WIFI_PS_MAX_MODEM ? "maximum" : "none");
}

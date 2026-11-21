#include "connected_power.h"

#include <esp32-hal-cpu.h>
#include <esp_wifi.h>

static bool g_connectedIdle = false;
static unsigned g_powerWork = 0;
static uint32_t g_activeCpuMhz = 0;
static wifi_ps_type_t g_savedWifiSleep = WIFI_PS_NONE;
static bool g_restoreWifiSleep = false;

static void applyConnectedCpu() {
    if (!g_activeCpuMhz) {
        uint32_t bootMhz = getCpuFrequencyMhz();
        // Classic ESP32 uses the 320 MHz PLL for both 80 and 160 MHz. Choose
        // that PLL before Wi-Fi starts, then avoid 240/80 PLL swaps at runtime.
        g_activeCpuMhz = bootMhz > 160 ? 160 : bootMhz;
    }
    uint32_t target = g_connectedIdle && !g_powerWork ? 80 : g_activeCpuMhz;
    if (getCpuFrequencyMhz() != target && !setCpuFrequencyMhz(target)) {
        Serial.printf("[power] CPU frequency change to %lu MHz failed\n",
                      static_cast<unsigned long>(target));
    }
}

void setConnectedPowerIdle(bool idle) {
    g_connectedIdle = idle;
    applyConnectedCpu();
}

bool connectedPowerIdle() { return g_connectedIdle; }

void beginConnectedPowerWork() {
    ++g_powerWork;
    applyConnectedCpu();
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
    applyConnectedCpu();
}

String connectedPowerStatus() {
    wifi_ps_type_t mode = WIFI_PS_NONE;
    bool known = esp_wifi_get_ps(&mode) == ESP_OK;
    return "power_idle=" + String(g_connectedIdle ? 1 : 0) +
           " cpu_mhz=" + String(getCpuFrequencyMhz()) +
           " wifi_sleep=" + String(!known ? "unavailable" : mode == WIFI_PS_MIN_MODEM
                                      ? "minimum" : mode == WIFI_PS_MAX_MODEM ? "maximum" : "none");
}

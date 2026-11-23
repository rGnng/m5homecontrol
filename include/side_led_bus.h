#pragma once

#include <utility/led/LED_Strip_Class.hpp>

#if M5UNIFIED_RMT_VERSION == 1
#include <Arduino.h>
#include <driver/rmt.h>
#include <vector>

// M5Unified's RMT bus only implements ESP-IDF 5+. Use the IDF 4 driver
// directly so a failed transmitter has a finite wait and can be reinitialized.
class SideLedBus : public m5::LedBus_Base {
public:
    explicit SideLedBus(int pin) : pin_(pin) {}
    ~SideLedBus() override { release(); }

    bool init() override {
        if (channel_ >= 0) return true;
        if (pin_ < 0 || pin_ >= GPIO_NUM_MAX ||
            !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)pin_)) return false;
        pinMode(pin_, OUTPUT);
        digitalWrite(pin_, LOW);
        rmt_channel_status_result_t status;
        if (rmt_get_channel_status(&status) != ESP_OK) return false;
        for (int i = 0; i < RMT_CHANNEL_MAX; ++i) {
            if (status.status[i] != RMT_CHANNEL_UNINIT) continue;
            auto channel = static_cast<rmt_channel_t>(i);
            rmt_config_t config = RMT_DEFAULT_CONFIG_TX(static_cast<gpio_num_t>(pin_), channel);
            config.clk_div = 2; // APB 80 MHz / 2 = 25 ns ticks.
            if (rmt_config(&config) != ESP_OK ||
                rmt_driver_install(channel, 0, ESP_INTR_FLAG_IRAM) != ESP_OK) continue;
            channel_ = i;
            return true;
        }
        return false;
    }

    void release() override {
        if (channel_ >= 0) {
            auto channel = static_cast<rmt_channel_t>(channel_);
            rmt_tx_stop(channel);
            // All writes below use wait_tx_done=false, so uninstall never
            // waits for the failed transmission's completion semaphore.
            rmt_driver_uninstall(channel);
            channel_ = -1;
        }
    }

    void write(const uint8_t* data, size_t length) override {
        if (!data || !length) return;
        if (channel_ < 0 && (uint32_t(millis() - lastFailure_) < 1000 || !init())) return;
        items_.resize(length * 8 + 1);
        size_t index = 0;
        for (size_t i = 0; i < length; ++i) {
            for (uint8_t mask = 0x80; mask; mask >>= 1) {
                auto& item = items_[index++];
                item.val = 0;
                item.level0 = 1;
                // SK6812 at 800 kHz: 0 = 300/900 ns, 1 = 600/600 ns.
                item.duration0 = (data[i] & mask) ? 24 : 12;
                item.duration1 = (data[i] & mask) ? 24 : 36;
            }
        }
        // Latch the frame with 80 microseconds low.
        auto& reset = items_[index];
        reset.val = 0;
        reset.duration0 = 1600;
        reset.duration1 = 1600;
        auto channel = static_cast<rmt_channel_t>(channel_);
        if (rmt_write_items(channel, items_.data(), items_.size(), false) != ESP_OK ||
            rmt_wait_tx_done(channel, pdMS_TO_TICKS(20)) != ESP_OK) {
            release();
            lastFailure_ = millis();
            Serial.println("[led] RMT transmit failed; retrying later");
        }
    }

private:
    int pin_;
    int channel_ = -1;
    uint32_t lastFailure_ = 0;
    std::vector<rmt_item32_t> items_;
};
#endif

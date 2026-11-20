#pragma once

#include <utility/led/LED_Strip_Class.hpp>

#if M5UNIFIED_RMT_VERSION == 1
#include <Arduino.h>
#include <esp32-hal-rmt.h>
#include <vector>

// M5Unified's RMT bus only implements ESP-IDF 5+. Use the Arduino 2.x
// RMT HAL on ESP-IDF 4, retaining M5.Led's color and brightness handling.
class SideLedBus : public m5::LedBus_Base {
public:
    explicit SideLedBus(int pin) : pin_(pin) {}
    ~SideLedBus() override { release(); }

    bool init() override {
        if (rmt_) return true;
        if (pin_ < 0 || pin_ >= GPIO_NUM_MAX ||
            !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)pin_)) return false;
        pinMode(pin_, OUTPUT);
        digitalWrite(pin_, LOW);
        rmt_ = rmtInit(pin_, RMT_TX_MODE, RMT_MEM_64);
        if (!rmt_) return false;
        if (rmtSetTick(rmt_, 25) != 25) {
            release();
            return false;
        }
        return true;
    }

    void release() override {
        if (rmt_) rmtDeinit(rmt_);
        rmt_ = nullptr;
    }

    void write(const uint8_t* data, size_t length) override {
        if (!rmt_ || !data || !length) return;
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
        if (!rmtWriteBlocking(rmt_, items_.data(), items_.size())) {
            Serial.println("[led] RMT transmit failed");
        }
    }

private:
    int pin_;
    rmt_obj_t* rmt_ = nullptr;
    std::vector<rmt_data_t> items_;
};
#endif

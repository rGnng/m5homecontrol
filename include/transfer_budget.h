#pragma once

#include <Arduino.h>
#include <atomic>
#include <cstddef>

// Shared by a request's transport, including HTTPClient's header/body readers.
// Stopping the transport makes library loops terminate even if the peer keeps
// its TCP connection open without finishing the response.
class TransferBudget {
public:
    TransferBudget(uint32_t milliseconds, size_t maxBytes,
                   const std::atomic<bool>* cancelled = nullptr)
        : started_(millis()), timeout_(milliseconds), maxBytes_(maxBytes), cancelled_(cancelled) {}

    bool allowed() {
        if (failed_ || (cancelled_ && cancelled_->load(std::memory_order_acquire)) ||
            uint32_t(millis() - started_) >= timeout_) {
            failed_ = true;
            return false;
        }
        return true;
    }
    void consumed(size_t bytes) {
        if (bytes > maxBytes_ - bytes_) failed_ = true;
        else bytes_ += bytes;
    }
    bool failed() const { return failed_; }
    void restart(uint32_t milliseconds, size_t maxBytes) {
        started_ = millis(); timeout_ = milliseconds; maxBytes_ = maxBytes;
        bytes_ = 0; failed_ = false;
    }

private:
    uint32_t started_, timeout_;
    size_t maxBytes_, bytes_ = 0;
    const std::atomic<bool>* cancelled_;
    bool failed_ = false;
};

template<class Transport>
class BudgetClient : public Transport {
public:
    explicit BudgetClient(TransferBudget& budget) : budget_(budget) {}
    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t* bytes, size_t length) override {
        return check() ? Transport::write(bytes, length) : 0;
    }
    int available() override { return check() ? Transport::available() : 0; }
    uint8_t connected() override { return check() ? Transport::connected() : 0; }
    int peek() override { return check() ? Transport::peek() : -1; }
    int read() override {
        if (!check()) return -1;
        uint8_t value;
        int count = Transport::read(&value, 1);
        if (count > 0) { budget_.consumed(1); return value; }
        return -1;
    }
    int read(uint8_t* bytes, size_t length) override {
        if (!check()) return -1;
        int count = Transport::read(bytes, length);
        if (count > 0) budget_.consumed(static_cast<size_t>(count));
        return count;
    }
private:
    bool check() {
        if (budget_.allowed()) return true;
        Transport::stop();
        return false;
    }
    TransferBudget& budget_;
};

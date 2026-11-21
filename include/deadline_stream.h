#pragma once

#include <Arduino.h>

// A stream's per-byte timeout does not bound an entire JSON download. This
// reader caps total time even when a server trickles data indefinitely.
template<class Client>
class DeadlineStream {
public:
    DeadlineStream(Client& client, uint32_t timeoutMs)
        : client_(client), startedAt_(millis()), timeoutMs_(timeoutMs) {}

    int read() {
        while (millis() - startedAt_ < timeoutMs_) {
            int value = client_.read();
            if (value >= 0) return value;
            if (!client_.connected()) return -1;
            delay(1);
        }
        return -1;
    }

    size_t readBytes(char* output, size_t length) {
        size_t count = 0;
        while (count < length) {
            int value = read();
            if (value < 0) break;
            output[count++] = static_cast<char>(value);
        }
        return count;
    }

private:
    Client& client_;
    uint32_t startedAt_;
    uint32_t timeoutMs_;
};

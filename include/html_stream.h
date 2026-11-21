#pragma once

#include <Arduino.h>
#include <cstring>

// Bound response memory independently of the number of pages or script size.
// Literal strings are copied directly into the small transmission buffer.
template<class Server, size_t ChunkSize = 1024>
class HtmlStream {
public:
    explicit HtmlStream(Server& server, void (*service)() = nullptr)
        : server_(server), service_(service) {}

    HtmlStream& operator+=(const char* text) {
        append(text, std::strlen(text));
        return *this;
    }

    HtmlStream& operator+=(const String& text) {
        append(text.c_str(), text.length());
        return *this;
    }

    void finish() {
        flush();
        server_.sendContent("", 0);
    }

private:
    static_assert(ChunkSize > 0, "HTML chunks must not be empty");
    Server& server_;
    void (*service_)();
    char buffer_[ChunkSize];
    size_t used_ = 0;

    void append(const char* text, size_t length) {
        while (length) {
            size_t count = length < ChunkSize - used_ ? length : ChunkSize - used_;
            std::memcpy(buffer_ + used_, text, count);
            used_ += count;
            text += count;
            length -= count;
            if (used_ == ChunkSize) flush();
        }
    }

    void flush() {
        if (!used_) return;
        server_.sendContent(buffer_, used_);
        used_ = 0;
        if (service_) service_();
    }
};

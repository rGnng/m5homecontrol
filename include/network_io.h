#pragma once

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lwip/sockets.h>
#include <cerrno>

// WiFiClient::write has a fixed, long retry loop in Arduino 2.x. Keep a
// back-pressured peer from monopolizing the main loop or HTTP worker.
inline size_t writeSocketWithin(int socket, const uint8_t* data, size_t length,
                                uint32_t timeoutMs = 250) {
    size_t sent = 0;
    uint32_t started = millis();
    while (socket >= 0 && sent < length && uint32_t(millis() - started) < timeoutMs) {
        int count = ::send(socket, data + sent, length - sent, MSG_DONTWAIT);
        if (count > 0) sent += static_cast<size_t>(count);
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) delay(1);
        else break;
    }
    return sent;
}

inline size_t writeClientWithin(WiFiClient& client, const uint8_t* data, size_t length) {
    size_t sent = writeSocketWithin(client.fd(), data, length);
    if (sent != length) client.stop();
    return sent;
}

class ResponsiveClient : public WiFiClient {
public:
    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t* data, size_t length) override {
        return writeClientWithin(*this, data, length);
    }
};

class ResponsiveSecureClient : public WiFiClientSecure {
public:
    int setTimeout(uint32_t seconds) override {
        int result = WiFiClientSecure::setTimeout(seconds);
        sslclient->socket_timeout = 250;
        return result;
    }
};

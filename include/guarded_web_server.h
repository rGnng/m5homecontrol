#pragma once

#include <WebServer.h>
#include "network_io.h"

class GuardedWebServer : public WebServer {
public:
    using WebServer::WebServer;
protected:
    size_t _currentClientWrite(const char* bytes, size_t length) override {
        return writeClientWithin(_currentClient, reinterpret_cast<const uint8_t*>(bytes), length);
    }
    size_t _currentClientWrite_P(PGM_P bytes, size_t length) override {
        // ESP32 flash is memory-mapped; no temporary copy is needed.
        return _currentClientWrite(bytes, length);
    }
};

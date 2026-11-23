#pragma once

#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "connected_power.h"
#include "network_io.h"
#include "transfer_budget.h"

// Only socket connection/handshake work leaves the main task. The WebSocket
// parser, callbacks, HA entities, and power guards remain main-task owned.
class AsyncWebSocket : public WebSocketsClient {
public:
    void setSuspended(bool suspended) {
        suspended_ = suspended;
        if (suspended) work_.cancelled.store(true, std::memory_order_release);
    }

    void pollConnect() {
        if (!connecting_ || !work_.done.load(std::memory_order_acquire)) return;
        connecting_ = false;
        endConnectedPowerWork();
        if (suspended_ || work_.cancelled.load() || !work_.ok) {
            delete work_.client; work_.client = nullptr;
            _lastConnectionFail = millis();
            return;
        }
        _client.tcp = work_.client;
        _client.ssl = work_.tls ? static_cast<WiFiClientSecure*>(work_.client) : nullptr;
        work_.client = nullptr;
        work_.budget.restart(100, SIZE_MAX);
        connectedCb();
        if (_client.tcp) _client.tcp->Stream::setTimeout(100);
        _lastConnectionFail = 0;
    }

    void loop() {
        pollConnect();
        if (suspended_ || connecting_ || !_port) return;
        work_.budget.restart(100, SIZE_MAX);
        if (!clientIsConnected(&_client)) {
            if (uint32_t(millis() - _lastConnectionFail) < _reconnectInterval) return;
            delete _client.tcp;
            _client.tcp = nullptr; _client.ssl = nullptr;
            if (!queue_) {
                queue_ = xQueueCreate(1, sizeof(ConnectWork*));
                if (!queue_ || xTaskCreate(connectTask, "ha-ws-connect", 12288, queue_, 1, nullptr) != pdPASS) {
                    if (queue_) vQueueDelete(queue_);
                    queue_ = nullptr; _lastConnectionFail = millis(); return;
                }
            }
            work_.host = _host; work_.port = _port; work_.tls = _client.isSSL;
            work_.client = nullptr; work_.ok = false;
            work_.cancelled.store(false); work_.done.store(false);
            work_.budget.restart(8000, SIZE_MAX);
            ConnectWork* request = &work_;
            if (xQueueSend(queue_, &request, 0) == pdTRUE) {
                connecting_ = true;
                beginConnectedPowerWork();
            } else _lastConnectionFail = millis();
            return;
        }
        // Calling the base loop here could reconnect synchronously if the
        // peer disconnects between the check above and that second check.
        handleClientData();
        if (_client.status == WSC_CONNECTED) {
            handleHBPing();
            handleHBTimeout(&_client);
        }
    }

private:
    struct ConnectWork {
        std::atomic<bool> done{false}, cancelled{false};
        TransferBudget budget{8000, SIZE_MAX, &cancelled};
        String host;
        uint16_t port = 0;
        bool tls = false, ok = false;
        WiFiClient* client = nullptr;
    } work_;
    QueueHandle_t queue_ = nullptr;
    bool connecting_ = false, suspended_ = false;

    static void connectTask(void* argument) {
        auto queue = static_cast<QueueHandle_t>(argument);
        for (;;) {
            ConnectWork* work = nullptr;
            if (xQueueReceive(queue, &work, portMAX_DELAY) != pdTRUE) continue;
            if (!work->cancelled.load()) {
                if (work->tls) {
                    auto* client = new BudgetClient<ResponsiveSecureClient>(work->budget);
                    client->setInsecure(); client->setHandshakeTimeout(2);
                    work->client = client;
                } else work->client = new BudgetClient<ResponsiveClient>(work->budget);
                if (work->client) {
                    work->ok = work->client->connect(work->host.c_str(), work->port, 1500) && work->budget.allowed();
                    work->client->setTimeout(1);
                }
            }
            work->done.store(true, std::memory_order_release);
        }
    }
};

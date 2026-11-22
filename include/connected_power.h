#pragma once

#include <Arduino.h>

// Connected idle only: this never suspends the ESP32 or its network connection.
void setConnectedPowerIdle(bool idle);
bool connectedPowerIdle();
void setConnectedPowerSuspended(bool suspended);
void beginConnectedPowerWork();
void endConnectedPowerWork();
String connectedPowerStatus();

class ConnectedPowerWork {
public:
    ConnectedPowerWork() { beginConnectedPowerWork(); }
    ~ConnectedPowerWork() { endConnectedPowerWork(); }
    ConnectedPowerWork(const ConnectedPowerWork&) = delete;
    ConnectedPowerWork& operator=(const ConnectedPowerWork&) = delete;
};

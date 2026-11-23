#pragma once

#include <Arduino.h>

enum class OffWake { Timer, Input, Error };

// Core Basic buttons (39/38/37) and optional Calculator3 IRQ (35), active low.
// The caller stops Wi-Fi, drains work and temporarily detaches the keypad ISR.
OffWake sleepWhileOff(bool keypadWake, uint32_t timerMs);
String offSleepStatus();

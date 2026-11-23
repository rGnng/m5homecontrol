#pragma once

#include <Arduino.h>

enum class OffWake { Timer, Input, DockChange, Error };

// Core Basic buttons (39/38/37) and optional Calculator3 IRQ (35), active low.
// The caller stops Wi-Fi, drains work and temporarily detaches the keypad ISR.
// dockLevel is the debounced GPIO26 level, or -1 to disable dock wake.
OffWake sleepWhileOff(bool keypadWake, uint32_t timerMs, int dockLevel = -1);
String offSleepStatus();

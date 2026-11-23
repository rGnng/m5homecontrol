#pragma once

#include <Arduino.h>

enum class DeepWake { None, Keypad, DockChange, Other };

// Called once after M5.begin(), before keypadInit(). Restores wake pins from RTC
// mode; true means the boot key must be read and discarded before normal input.
bool deepSleepBoot();

// Requires LCD/Wi-Fi off, drained HTTP work, quiet audio and a detached keypad
// ISR. Success never returns; failure returns false so the UI can recover.
// Optional GPIO26 change wake in addition to the keypad; no timer wakes.
bool enterDeepSleep(int dockLevel = -1, bool night = false);
DeepWake deepSleepWakeCause();
int deepSleepDockLevel();
bool deepSleepWasNight();
String deepSleepStatus();

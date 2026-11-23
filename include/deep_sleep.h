#pragma once

#include <Arduino.h>

// Called once after M5.begin(), before keypadInit(). Restores GPIO35 from RTC
// mode; true means the boot key must be read and discarded before normal input.
bool deepSleepBoot();

// Requires LCD/Wi-Fi off, drained HTTP work, quiet audio and a detached keypad
// ISR. Success never returns; failure returns false so the UI can recover.
// Keypad wake only; no periodic timer wakes.
bool enterDeepSleep();
String deepSleepStatus();

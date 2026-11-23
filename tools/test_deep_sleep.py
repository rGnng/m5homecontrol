#!/usr/bin/env python3
"""Run the actual deep-sleep module against RTC/GPIO/sleep stubs."""
import ast
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
module = ast.parse((ROOT/'tools/test_controls.py').read_text())
arduino = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'arduino' for t in n.targets))
arduino += '\nstruct SerialStub{void flush(){}};inline SerialStub Serial;\n'
gpio = r'''
#pragma once
#include <cassert>
inline constexpr int GPIO_NUM_35=35,ESP_OK=0,ESP_FAIL=-1;
inline int irqLevel=1,rtcRestores=0;
inline int gpio_get_level(int pin){assert(pin==35);return irqLevel;}
inline int rtc_gpio_deinit(int pin){assert(pin==35);++rtcRestores;return ESP_OK;}
'''
sleep = r'''
#pragma once
#include "driver/gpio.h"
inline constexpr int ESP_SLEEP_WAKEUP_ALL=0,ESP_SLEEP_WAKEUP_EXT0=2,ESP_SLEEP_WAKEUP_TIMER=4;
inline bool ext0=false,timer=false,failExt0=false;
inline int wakeCause=0,deepEntries=0;
inline int esp_sleep_enable_ext0_wakeup(int pin,int level){assert(pin==35 && level==0);if(failExt0)return ESP_FAIL;ext0=true;return ESP_OK;}
inline int esp_sleep_disable_wakeup_source(int source){
    if(source==ESP_SLEEP_WAKEUP_EXT0 || source==ESP_SLEEP_WAKEUP_ALL)ext0=false;
    if(source==ESP_SLEEP_WAKEUP_TIMER || source==ESP_SLEEP_WAKEUP_ALL)timer=false;
    return ESP_OK;
}
inline int esp_sleep_get_wakeup_cause(){return wakeCause;}
struct DeepSleep{};
[[noreturn]] inline void esp_deep_sleep_start(){assert(ext0);++deepEntries;throw DeepSleep{};}
'''
system = r'''
#pragma once
inline constexpr int ESP_RST_POWERON=1,ESP_RST_SW=3,ESP_RST_DEEPSLEEP=5;
inline int resetReason=ESP_RST_POWERON;
inline int esp_reset_reason(){return resetReason;}
'''
tests = r'''
#include "deep_sleep.h"
#include <esp_sleep.h>
#include <esp_system.h>
#include <cstdio>
static void clean(){assert(!ext0 && !timer);}
static void enter(){
    try{enterDeepSleep();assert(false);}catch(const DeepSleep&){}
    assert(ext0 && !timer);
}
int main(){
    assert(!deepSleepBoot());clean();
    assert(deepSleepStatus().find("deep_sleep_entries=0")!=String::npos);
    irqLevel=0;assert(!enterDeepSleep() && deepEntries==0);clean();
    irqLevel=1;failExt0=true;assert(!enterDeepSleep());clean();
    failExt0=false;assert(deepEntries==0);
    assert(deepSleepStatus().find("deep_sleep_errors=1")!=String::npos);
    // Production deep sleep clears an earlier light-sleep health timer.
    timer=true;enter();assert(deepEntries==1 && !timer);
    resetReason=ESP_RST_DEEPSLEEP;wakeCause=ESP_SLEEP_WAKEUP_EXT0;
    assert(deepSleepBoot() && rtcRestores==1);clean();
    assert(deepSleepStatus().find("deep_sleep_entries=1 deep_sleep_keypad_wakes=1 deep_sleep_timer_wakes=0")!=String::npos);
    assert(deepSleepStatus().find("deep_sleep_last_wake=keypad")!=String::npos);
    // The boot key is requested for consumption once, never replayed as an action.
    assert(!deepSleepBoot() && rtcRestores==1);
    timer=true;enter();assert(!timer);
    assert(deepSleepBoot() && rtcRestores==2);clean();
    assert(deepSleepStatus().find("deep_sleep_entries=2 deep_sleep_keypad_wakes=2")!=String::npos);
    // A software reset is never reported as a successful deep-sleep wake.
    enter();resetReason=ESP_RST_SW;
    assert(!deepSleepBoot());
    assert(deepSleepStatus().find("deep_sleep_entries=0 deep_sleep_keypad_wakes=0 deep_sleep_timer_wakes=0")!=String::npos);
    assert(deepSleepStatus().find("deep_sleep_last_wake=none")!=String::npos);
    std::puts("PASS: actual EXT0 keypad wake without timer, stale-timer cleanup, setup failure, pending IRQ, retained counters and one-time boot-key consumption");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-deep-sleep-') as directory:
    tmp = Path(directory)
    (tmp/'driver').mkdir()
    (tmp/'Arduino.h').write_text(arduino)
    (tmp/'driver/gpio.h').write_text(gpio)
    (tmp/'driver/rtc_io.h').write_text('#include "gpio.h"\n')
    (tmp/'esp_sleep.h').write_text(sleep)
    (tmp/'esp_system.h').write_text(system)
    (tmp/'esp_attr.h').write_text('#define RTC_DATA_ATTR\n')
    (tmp/'test.cpp').write_text(tests)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{tmp}',f'-I{ROOT/"include"}',
                    str(ROOT/'src/deep_sleep.cpp'),str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)

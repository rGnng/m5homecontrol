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
inline constexpr int GPIO_NUM_35=35,GPIO_NUM_26=26,ESP_OK=0,ESP_FAIL=-1;
inline constexpr int RTC_GPIO_MODE_INPUT_ONLY=1,GPIO_MODE_INPUT=1;
inline int irqLevel=1,dockLevel=1,rtcRestores=0,dockRestores=0;
inline bool rtcPullup=false,failRtc=false;
inline int gpio_get_level(int pin){assert(pin==35 || pin==26);return pin==35?irqLevel:dockLevel;}
inline int rtc_gpio_deinit(int pin){assert(pin==35 || pin==26);if(pin==35)++rtcRestores;else{++dockRestores;rtcPullup=false;}return ESP_OK;}
inline int rtc_gpio_init(int pin){assert(pin==26);return failRtc?ESP_FAIL:ESP_OK;}
inline int rtc_gpio_set_direction(int pin,int mode){assert(pin==26 && mode==RTC_GPIO_MODE_INPUT_ONLY);return ESP_OK;}
inline int rtc_gpio_pulldown_dis(int pin){assert(pin==26);return ESP_OK;}
inline int rtc_gpio_pullup_en(int pin){assert(pin==26);rtcPullup=true;return ESP_OK;}
inline int gpio_set_direction(int pin,int mode){assert(pin==26 && mode==GPIO_MODE_INPUT);return ESP_OK;}
inline int gpio_pullup_en(int pin){assert(pin==26);return ESP_OK;}
'''
sleep = r'''
#pragma once
#include "driver/gpio.h"
inline constexpr int ESP_SLEEP_WAKEUP_ALL=0,ESP_SLEEP_WAKEUP_EXT0=2,ESP_SLEEP_WAKEUP_EXT1=3,ESP_SLEEP_WAKEUP_TIMER=4;
inline constexpr int ESP_EXT1_WAKEUP_ANY_HIGH=1,ESP_EXT1_WAKEUP_ALL_LOW=0;
inline bool ext0=false,ext1=false,timer=false,failExt0=false,failExt1=false;
inline int ext1Mode=-1;
inline int wakeCause=0,deepEntries=0;
inline int esp_sleep_enable_ext0_wakeup(int pin,int level){assert(pin==35 && level==0);if(failExt0)return ESP_FAIL;ext0=true;return ESP_OK;}
inline int esp_sleep_enable_ext1_wakeup(uint64_t mask,int mode){assert(mask==(1ULL<<26));if(failExt1)return ESP_FAIL;ext1=true;ext1Mode=mode;return ESP_OK;}
inline int esp_sleep_disable_wakeup_source(int source){
    if(source==ESP_SLEEP_WAKEUP_EXT0 || source==ESP_SLEEP_WAKEUP_ALL)ext0=false;
    if(source==ESP_SLEEP_WAKEUP_EXT1 || source==ESP_SLEEP_WAKEUP_ALL)ext1=false;
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
static void clean(){assert(!ext0 && !ext1 && !timer);}
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
    // GPIO26 complements keypad EXT0 with opposite-level EXT1 and an RTC pull-up.
    resetReason=ESP_RST_POWERON;assert(!deepSleepBoot());
    dockLevel=1;
    try{enterDeepSleep(1,true);assert(false);}catch(const DeepSleep&){}
    assert(ext0 && ext1 && !timer && rtcPullup && ext1Mode==ESP_EXT1_WAKEUP_ALL_LOW);
    resetReason=ESP_RST_DEEPSLEEP;wakeCause=ESP_SLEEP_WAKEUP_EXT1;dockLevel=0;
    assert(!deepSleepBoot());clean();
    assert(deepSleepWakeCause()==DeepWake::DockChange && deepSleepDockLevel()==1 && deepSleepWasNight());
    assert(deepSleepStatus().find("deep_sleep_dock_wakes=1")!=String::npos);
    assert(deepSleepStatus().find("deep_sleep_last_wake=dock")!=String::npos);
    try{enterDeepSleep(0,false);assert(false);}catch(const DeepSleep&){}
    assert(ext1Mode==ESP_EXT1_WAKEUP_ANY_HIGH && rtcPullup);
    dockLevel=1;assert(!deepSleepBoot());clean();
    assert(deepSleepDockLevel()==0 && !deepSleepWasNight());
    assert(deepSleepStatus().find("deep_sleep_undock_wakes=1")!=String::npos);
    assert(deepSleepStatus().find("deep_sleep_last_wake=undock")!=String::npos);
    int restores=dockRestores;failExt1=true;
    assert(!enterDeepSleep(1));clean();assert(dockRestores==restores+1 && !rtcPullup);
    failExt1=false;failRtc=true;assert(!enterDeepSleep(1));clean();failRtc=false;
    dockLevel=0;int entries=deepEntries;assert(!enterDeepSleep(1) && deepEntries==entries);clean();
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

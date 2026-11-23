#!/usr/bin/env python3
"""Exercise actual light-sleep code and main-loop integration with ESP32 stubs."""
import ast
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
module = ast.parse((ROOT / 'tools/test_controls.py').read_text())
arduino = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'arduino' for t in n.targets))
arduino += r'''
inline uint32_t now=10000;
inline uint32_t millis(){return now;}
inline void delay(unsigned ms){now+=ms;}
struct SerialStub{void flush(){}void println(const char*){}};
inline SerialStub Serial;
'''
gpio = r'''
#pragma once
#include <cassert>
#include <set>
using gpio_num_t=int;
inline constexpr int GPIO_NUM_39=39,GPIO_NUM_38=38,GPIO_NUM_37=37,GPIO_NUM_35=35,GPIO_NUM_26=26;
inline constexpr int GPIO_INTR_LOW_LEVEL=0,GPIO_INTR_HIGH_LEVEL=1,ESP_OK=0,ESP_FAIL=-1;
inline std::set<int> lowPins,armedPins;
inline int failPin=-1;
inline int dockMode=-1;
inline int gpio_get_level(int pin){return lowPins.count(pin)?0:1;}
inline int gpio_wakeup_enable(int pin,int mode){if(pin==26)dockMode=mode;else assert(mode==GPIO_INTR_LOW_LEVEL);if(pin==failPin)return ESP_FAIL;armedPins.insert(pin);return ESP_OK;}
inline int gpio_wakeup_disable(int pin){assert(armedPins.erase(pin)==1);return ESP_OK;}
'''
sleep = r'''
#pragma once
#include "driver/gpio.h"
using esp_err_t=int;
inline constexpr int ESP_SLEEP_WAKEUP_GPIO=7,ESP_SLEEP_WAKEUP_TIMER=4;
inline bool gpioEnabled=false,timerEnabled=false,failGpio=false,failTimer=false;
inline int sleepResult=ESP_OK,wakeCause=ESP_SLEEP_WAKEUP_TIMER,sleeps=0;
inline uint64_t timerMicros=0;
inline int64_t microsNow=0;
inline std::set<int> pinsAtSleep;
inline int dockAtWake=-1;
inline int esp_sleep_enable_gpio_wakeup(){if(failGpio)return ESP_FAIL;gpioEnabled=true;return ESP_OK;}
inline int esp_sleep_enable_timer_wakeup(uint64_t us){if(failTimer)return ESP_FAIL;timerEnabled=true;timerMicros=us;return ESP_OK;}
inline int esp_sleep_disable_wakeup_source(int source){if(source==ESP_SLEEP_WAKEUP_GPIO)gpioEnabled=false;else{assert(source==ESP_SLEEP_WAKEUP_TIMER);timerEnabled=false;}return ESP_OK;}
inline int esp_light_sleep_start(){assert(gpioEnabled && timerEnabled);++sleeps;pinsAtSleep=armedPins;microsNow+=timerMicros;now+=timerMicros/1000;if(dockAtWake==0)lowPins.insert(26);else if(dockAtWake==1)lowPins.erase(26);return sleepResult;}
inline int esp_sleep_get_wakeup_cause(){return wakeCause;}
'''
hardware_tests = r'''
#include "off_sleep.h"
#include <esp_sleep.h>
#include <cstdio>
static void clean(){assert(armedPins.empty() && !gpioEnabled && !timerEnabled);}
int main(){
    assert(sleepWhileOff(true,1000)==OffWake::Timer);clean();
    assert(pinsAtSleep==std::set<int>({35,37,38,39}) && timerMicros==1000000);
    assert(offSleepStatus().find("light_sleep_entries=1 light_sleep_ms=1000 light_sleep_errors=0")!=String::npos);
    wakeCause=ESP_SLEEP_WAKEUP_GPIO;
    assert(sleepWhileOff(false,60000)==OffWake::Input);clean();
    assert(pinsAtSleep==std::set<int>({37,38,39}));
    assert(offSleepStatus().find("light_sleep_last_wake=input")!=String::npos);
    int calls=sleeps;
    // An input already low must be handled before sleeping (entry race).
    for(int pin:{35,37,38,39}){
        lowPins={pin};assert(sleepWhileOff(true,1000)==OffWake::Input);clean();assert(sleeps==calls);
    }
    // Faulty keypad IRQ is excluded; Core buttons and timer still wake.
    lowPins={35};assert(sleepWhileOff(false,1000)==OffWake::Input);clean();assert(sleeps==calls+1);
    lowPins.clear();calls=sleeps;
    // Failed setup cleans partial configuration; rejected sleep keeps polling.
    for(int pin:{35,37,38,39}){
        failPin=pin;assert(sleepWhileOff(true,1000)==OffWake::Error);clean();assert(sleeps==calls);
    }
    failPin=-1;failGpio=true;assert(sleepWhileOff(true,1000)==OffWake::Error);clean();
    failGpio=false;failTimer=true;assert(sleepWhileOff(true,1000)==OffWake::Error);clean();
    failTimer=false;sleepResult=ESP_FAIL;assert(sleepWhileOff(true,1000)==OffWake::Error);clean();
    assert(offSleepStatus().find("light_sleep_errors=7")!=String::npos);
    sleepResult=ESP_OK;wakeCause=ESP_SLEEP_WAKEUP_GPIO;dockAtWake=0;
    assert(sleepWhileOff(true,1000,1)==OffWake::DockChange);clean();
    assert(dockMode==GPIO_INTR_LOW_LEVEL && pinsAtSleep.count(26));
    dockAtWake=1;
    assert(sleepWhileOff(true,1000,0)==OffWake::DockChange);clean();
    assert(dockMode==GPIO_INTR_HIGH_LEVEL);
    lowPins={26};calls=sleeps;
    assert(sleepWhileOff(true,1000,1)==OffWake::DockChange && sleeps==calls);clean();
    lowPins={26,35};assert(sleepWhileOff(true,1000,1)==OffWake::Input);clean();
    lowPins.clear();dockAtWake=-1;failPin=26;
    assert(sleepWhileOff(true,1000,1)==OffWake::Error);clean();
    failPin=-1;
    std::puts("PASS: actual GPIO/timer sleep, all wake pins, pending-input race, partial setup cleanup and rejected sleep");
}
'''
integration_prefix = r'''
#include "backlight_schedule.h"
#include "app_config.h"
#include "deep_sleep.h"
#include "off_sleep.h"
#include <cassert>
#include <atomic>
#include <cstdio>
namespace m5{enum class board_t{board_M5Stack,other};}
struct{m5::board_t board=m5::board_t::board_M5Stack;m5::board_t getBoard(){return board;}}M5;
static BacklightPhase g_backlightPhase=BacklightPhase::Off;
static uint32_t g_offStartedAt=0,g_lastKeyPoll=0;
static bool busy=false,work=false,g_audioQuiet=true,net=false,netOk=true;
static int deepCalls=0;
bool enterDeepSleep(int,bool){++deepCalls;return false;}
static bool g_resumeDeepSleep=false,dockPending=false;
static bool isDocked=false;
bool dockConnected(){return isDocked;}
static bool dockInputPending(){return dockPending;}
static int dockInputLevel(){return 1;}
AppConfig g_config;
static bool g_scheduleNight=false;
static std::atomic<bool> g_commandLedActive{false};
static bool g_keypadIrqAvailable=true,g_keypadIrqVerified=false,g_keypadIrqFault=false;
static constexpr int KEYPAD_IRQ_PIN=35,KEYPAD_IRQ_HEALTH_MS=1000,FALLING=2,LOW=0;
static uint8_t g_wakeOnlyButtons=0;
static int pressedPin=-1,detaches=0,attaches=0,polls=0,wakes=0,sleepCalls=0;
static uint32_t sleepMs=0;
static OffWake result=OffWake::Timer;
static bool haHttpBusy(){return busy;}
static bool connectedPowerWorkActive(){return work;}
bool networkSetSuspended(bool value){if(!netOk)return false;net=value;return true;}
static int digitalRead(int pin){return pin==pressedPin?LOW:1;}
static int digitalPinToInterrupt(int pin){return pin;}
static void keypadInterrupt(){}
static void detachInterrupt(int pin){assert(pin==35);++detaches;}
static void attachInterrupt(int pin,void(*handler)(),int mode){assert(pin==35 && handler==keypadInterrupt && mode==FALLING);++attaches;}
static char keypadPoll(){assert(now-g_lastKeyPoll>=1000);++polls;return 'D';}
static void noteActivity(){++wakes;net=false;g_backlightPhase=BacklightPhase::Normal;}
OffWake sleepWhileOff(bool keypad,uint32_t ms,int dock){assert(dock==1);assert(net && !busy && !work);assert(keypad==(g_keypadIrqAvailable && !g_keypadIrqFault));++sleepCalls;sleepMs=ms;return result;}
'''
integration_tests = r'''
int main(){
    // Normal/Dim, early Off, active HTTP/OTA, sound and LEDs may not sleep.
    for(auto phase:{BacklightPhase::Normal,BacklightPhase::Dim}){g_backlightPhase=phase;offSleepTick();}
    g_backlightPhase=BacklightPhase::Off;now=499;offSleepTick();now=10000;
    isDocked=true;offSleepTick();assert(sleepCalls==0 && deepCalls==0);isDocked=false;
    busy=true;offSleepTick();busy=false;work=true;offSleepTick();work=false;
    g_audioQuiet=false;offSleepTick();g_audioQuiet=true;
    g_commandLedActive=true;offSleepTick();g_commandLedActive=false;
    assert(sleepCalls==0 && !net);
    netOk=false;offSleepTick();assert(sleepCalls==0);netOk=true;
    // Timer wakes keep both Wi-Fi and LCD off; never consume an action.
    offSleepTick();assert(sleepMs==1000 && net && wakes==0 && polls==0);
    assert(detaches==1 && attaches==1 && g_backlightPhase==BacklightPhase::Off);
    offSleepTick();assert(net && wakes==0 && sleepCalls==2);
    g_keypadIrqVerified=true;offSleepTick();assert(sleepMs==60000 && net && wakes==0);
    result=OffWake::Error;offSleepTick();assert(net && wakes==0 && detaches==attaches);
    // Keypad input is consumed while Off, then resumes networking.
    result=OffWake::Input;offSleepTick();assert(!net && wakes==1 && polls==1 && g_wakeOnlyButtons==0);
    // A held Core button remains wake-only through its debounce/release.
    g_backlightPhase=BacklightPhase::Off;pressedPin=38;offSleepTick();
    assert(!net && wakes==2 && polls==2 && g_wakeOnlyButtons==2);
    g_backlightPhase=BacklightPhase::Off;g_keypadIrqFault=true;result=OffWake::Timer;offSleepTick();
    g_keypadIrqAvailable=false;offSleepTick();assert(detaches==attaches && sleepCalls==8);
    M5.board=m5::board_t::other;offSleepTick();assert(sleepCalls==8);
    // Each saved period selects Deep independently, without timer wake.
    M5.board=m5::board_t::board_M5Stack;g_keypadIrqAvailable=true;
    g_keypadIrqFault=false;g_config.daySleepMode=SleepMode::Deep;
    g_backlightPhase=BacklightPhase::Off;offSleepTick();
    assert(deepCalls==1 && !net);
    g_scheduleNight=true;g_backlightPhase=BacklightPhase::Off;offSleepTick();
    assert(deepCalls==1 && sleepCalls==9 && net); // Night remains Light.
    g_config.nightSleepMode=SleepMode::Deep;offSleepTick();
    assert(deepCalls==2 && !net);
    // Missing/faulty keypad cannot leave the device stuck in scheduled deep sleep.
    g_backlightPhase=BacklightPhase::Off;g_keypadIrqAvailable=false;offSleepTick();
    assert(deepCalls==2 && sleepCalls==10 && net);
    // A dock wake leaves display/network asleep until debounce selects Dim.
    result=OffWake::DockChange;offSleepTick();assert(wakes==4 && net);
    int sleepsBefore=sleepCalls;dockPending=true;offSleepTick();assert(sleepCalls==sleepsBefore);
    dockPending=false;g_keypadIrqAvailable=true;g_config.daySleepMode=SleepMode::Light;
    g_scheduleNight=false;g_resumeDeepSleep=true;offSleepTick();assert(deepCalls==3);
    std::puts("PASS: actual Off orchestration, transfer/audio/LED guards, radio stop, timer-only suspension, fallback polling and wake-only input");
}
'''
source = (ROOT / 'src/main.cpp').read_text()
start = source.index('static void offSleepTick() {')
end = source.index('\nvoid loop()', start)
with tempfile.TemporaryDirectory(prefix='m5-off-sleep-') as directory:
    tmp = Path(directory)
    (tmp/'driver').mkdir()
    (tmp/'Arduino.h').write_text(arduino)
    (tmp/'driver/gpio.h').write_text(gpio)
    (tmp/'esp_sleep.h').write_text(sleep)
    (tmp/'esp_timer.h').write_text('#pragma once\n#include <esp_sleep.h>\ninline int64_t esp_timer_get_time(){return microsNow;}\n')
    for name, code, extra in [('hardware', hardware_tests, [str(ROOT/'src/off_sleep.cpp')]),
                              ('integration', integration_prefix+source[start:end]+integration_tests, [])]:
        (tmp/f'{name}.cpp').write_text(code)
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{tmp}',f'-I{ROOT/"include"}',
                        *extra,str(tmp/f'{name}.cpp'),'-o',str(tmp/name)],check=True)
        subprocess.run([str(tmp/name)],check=True)

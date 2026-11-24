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
#include <map>
using gpio_num_t=int;
inline constexpr int GPIO_NUM_39=39,GPIO_NUM_38=38,GPIO_NUM_37=37,GPIO_NUM_35=35,GPIO_NUM_26=26;
inline constexpr int GPIO_INTR_DISABLE=0,GPIO_INTR_LOW_LEVEL=4,GPIO_INTR_HIGH_LEVEL=5,ESP_OK=0,ESP_FAIL=-1;
inline std::set<int> lowPins,armedPins;
inline std::map<int,int> interruptTypes;
inline std::set<int> cpuInterrupts;
inline int failPin=-1;
inline int dockMode=-1;
inline int gpio_get_level(int pin){return lowPins.count(pin)?0:1;}
inline int gpio_wakeup_enable(int pin,int mode){if(pin==26)dockMode=mode;else assert(mode==GPIO_INTR_LOW_LEVEL);if(pin==failPin)return ESP_FAIL;interruptTypes[pin]=mode;armedPins.insert(pin);return ESP_OK;}
inline int gpio_wakeup_disable(int pin){armedPins.erase(pin);return ESP_OK;}
inline int gpio_intr_disable(int pin){cpuInterrupts.erase(pin);return ESP_OK;}
inline int gpio_set_intr_type(int pin,int mode){interruptTypes[pin]=mode;return ESP_OK;}
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
#include "sleep_sequence.h"
#include "power_status.h"
#include <esp_sleep.h>
#include <cstdio>
static void clean(){
    assert(armedPins.empty() && !gpioEnabled && !timerEnabled && cpuInterrupts.empty());
    for(auto entry:interruptTypes)assert(entry.second==GPIO_INTR_DISABLE);
}
int main(){
    // A software/watchdog restart can preserve level triggers. Arduino's
    // pinMode() would retain these and gpio_config() re-enable the interrupt.
    for(int pin:{26,35,37,38,39}){
        interruptTypes[pin]=GPIO_INTR_LOW_LEVEL;cpuInterrupts.insert(pin);armedPins.insert(pin);
    }
    resetOffSleepWakePins();clean();
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
#include "sleep_sequence.h"
#include "power_status.h"
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
static SleepSequence g_sleepSequence;
static PowerStatus power;
static bool usbOnRefresh=false;
PowerStatus readPowerStatus(bool force){if(force && usbOnRefresh)power.coreInput=1;return power;}
static bool isDocked=false;
bool dockConnected(){return isDocked;}
static bool dockInputPending(){return dockPending;}
static int dockInputLevel(){return 1;}
AppConfig g_config;
static bool g_scheduleNight=false;
static std::atomic<bool> g_commandLedActive{false};
static std::atomic<bool> g_lowBatteryLedPending{false};
static bool g_keypadIrqAvailable=true,g_keypadIrqVerified=false,g_keypadIrqFault=false;
static constexpr int KEYPAD_IRQ_PIN=35,KEYPAD_IRQ_HEALTH_MS=1000,FALLING=2,LOW=0;
static uint8_t g_wakeOnlyButtons=0;
static int pressedPin=-1,detaches=0,attaches=0,polls=0,wakes=0,sleepCalls=0;
static uint32_t sleepMs=0;
static OffWake result=OffWake::Timer;
static bool haHttpBusy(){return busy;}
static bool connectedPowerWorkActive(){return work;}
bool networkSetSuspended(bool value){if(!netOk)return false;net=value;return true;}
bool networkSuspended(){return net;}
static int digitalRead(int pin){return pin==pressedPin?LOW:1;}
static int digitalPinToInterrupt(int pin){return pin;}
static void keypadInterrupt(){}
static void detachInterrupt(int pin){assert(pin==35);++detaches;}
static void attachInterrupt(int pin,void(*handler)(),int mode){assert(pin==35 && handler==keypadInterrupt && mode==FALLING);++attaches;}
static char keypadPoll(){assert(now-g_lastKeyPoll>=1000);++polls;return 'D';}
static void noteActivity(){++wakes;net=false;g_backlightPhase=BacklightPhase::Normal;}
OffWake sleepWhileOff(bool keypad,uint32_t ms,int dock){assert(dock==1);assert(net && !busy && !work);assert(keypad==(g_keypadIrqAvailable && !g_keypadIrqFault));++sleepCalls;sleepMs=ms;if(result==OffWake::Timer)now+=ms;return result;}
'''
integration_tests = r'''
static void reset(){
    now=0;g_offStartedAt=0;g_sleepSequence.reset();g_config=AppConfig{};
    g_backlightPhase=BacklightPhase::Off;g_resumeDeepSleep=dockPending=isDocked=false;
    busy=work=net=false;g_audioQuiet=netOk=true;power=PowerStatus{};power.coreInput=0;
    usbOnRefresh=false;
    g_keypadIrqAvailable=true;g_keypadIrqVerified=g_keypadIrqFault=false;
    pressedPin=-1;detaches=attaches=polls=wakes=sleepCalls=deepCalls=0;
    g_commandLedActive=g_lowBatteryLedPending=false;g_wakeOnlyButtons=0;
    result=OffWake::Timer;M5.board=m5::board_t::board_M5Stack;g_scheduleNight=false;
}
int main(){
    reset();now=59999;offSleepTick();assert(sleepCalls==0 && !net);
    now=60000;offSleepTick();assert(sleepCalls==1 && sleepMs==1000 && now==61000 && net);
    assert(g_sleepSequence.lightStarted() && wakes==0 && polls==0 && detaches==attaches);
    // Deep sleep starts from actual Light entry, not from screen Off. A short
    // final timer reaches the exact deadline instead of oversleeping a health poll.
    g_keypadIrqVerified=true;now=1860000-250;offSleepTick();
    assert(now==1860000 && sleepMs==250 && deepCalls==0);
    offSleepTick();assert(deepCalls==1 && !net && wakes==1 && polls==1);
    offSleepTick();assert(!g_sleepSequence.lightStarted());
    // Disabled Light blocks the whole chain; disabled Deep permits indefinite Light.
    reset();g_config.lightSleepMinutes=0;now=86400000;offSleepTick();assert(!net && sleepCalls==0 && deepCalls==0);
    reset();g_config.deepSleepMinutes=0;now=60000;offSleepTick();now=86400000;offSleepTick();
    assert(sleepCalls==2 && deepCalls==0 && net);
    // USB power and unknown readings cannot enter sleep. If detected on a
    // timer wake, restore web access without waking the display.
    for(int input:{-1,1}){
        reset();power.coreInput=input;now=60000;offSleepTick();assert(sleepCalls==0 && deepCalls==0 && !net);
        g_sleepSequence.screenOff(0);g_sleepSequence.startLight(0);net=true;
        offSleepTick();assert(!net && !g_sleepSequence.lightStarted() && g_backlightPhase==BacklightPhase::Off);
    }
    reset();isDocked=true;now=60000;offSleepTick();assert(sleepCalls==0 && deepCalls==0 && !net);
    reset();usbOnRefresh=true;now=60000;offSleepTick();assert(sleepCalls==0 && deepCalls==0 && !net);
    reset();now=60000;offSleepTick();usbOnRefresh=true;now=1860000;offSleepTick();
    assert(deepCalls==0 && !net && !g_sleepSequence.lightStarted());
    // Every existing transfer/audio/LED guard still delays entry. No deep
    // countdown runs while a transfer holds the device awake.
    reset();now=60000;
    busy=true;offSleepTick();busy=false;work=true;offSleepTick();work=false;
    g_audioQuiet=false;offSleepTick();g_audioQuiet=true;
    g_commandLedActive=true;offSleepTick();g_commandLedActive=false;
    g_lowBatteryLedPending=true;offSleepTick();g_lowBatteryLedPending=false;
    dockPending=true;offSleepTick();dockPending=false;
    assert(!net && !g_sleepSequence.lightStarted() && sleepCalls==0);
    netOk=false;offSleepTick();assert(!net && sleepCalls==0);netOk=true;
    now=90000;offSleepTick();assert(g_sleepSequence.lightStarted());
    now=1889999;assert(!g_sleepSequence.deepDue(now,30));
    now=1890000;assert(g_sleepSequence.deepDue(now,30));
    // Day/night changes do not restart the shared sleep plan.
    reset();now=60000;offSleepTick();g_scheduleNight=true;now=100000;offSleepTick();
    assert(!g_sleepSequence.deepDue(1859999,30) && g_sleepSequence.deepDue(1860000,30));
    // Failed first Light entry does not count as time spent asleep.
    reset();now=60000;result=OffWake::Error;offSleepTick();assert(!g_sleepSequence.lightStarted() && detaches==attaches);
    now=70000;result=OffWake::Timer;offSleepTick();assert(!g_sleepSequence.deepDue(1860000,30));
    assert(g_sleepSequence.deepDue(1870000,30));
    // Missing/faulty keypad IRQ retains Light; it must never select unwakeable Deep.
    for(bool missing:{false,true}){
        reset();g_keypadIrqAvailable=!missing;g_keypadIrqFault=!missing;now=60000;offSleepTick();
        now=1860000;offSleepTick();assert(deepCalls==0 && sleepCalls==2 && detaches==attaches);
    }
    // A key remains wake-only, including a held Core B button.
    reset();now=60000;pressedPin=38;result=OffWake::Input;offSleepTick();
    assert(!net && wakes==1 && polls==1 && g_wakeOnlyButtons==2 && !g_sleepSequence.lightStarted());
    reset();now=60000;result=OffWake::DockChange;offSleepTick();assert(net && wakes==0);
    dockPending=true;int calls=sleepCalls;offSleepTick();assert(sleepCalls==calls);
    // Debounced ghost dock wakes can return to Deep without replaying the full chain.
    reset();now=1000;g_resumeDeepSleep=true;offSleepTick();assert(deepCalls==1 && wakes==1);
    // Normal/Dim and other boards never sleep; a new Off interval resets Light.
    reset();now=60000;offSleepTick();g_backlightPhase=BacklightPhase::Dim;offSleepTick();assert(!g_sleepSequence.lightStarted());
    g_backlightPhase=BacklightPhase::Normal;offSleepTick();M5.board=m5::board_t::other;
    g_backlightPhase=BacklightPhase::Off;offSleepTick();assert(sleepCalls==1);
    // Both deadlines survive millis rollover and support the maximum 24-hour delay.
    reset();g_offStartedAt=UINT32_MAX-30000;now=g_offStartedAt+59999;offSleepTick();assert(sleepCalls==0);
    now=g_offStartedAt+60000;offSleepTick();assert(sleepCalls==1);
    uint32_t lightAt=now-1000;assert(!g_sleepSequence.deepDue(lightAt+1799999,30));
    assert(g_sleepSequence.deepDue(lightAt+1800000,30));
    SleepSequence sequence;sequence.screenOff(123);assert(!sequence.lightDue(123+86399999,1440));
    assert(sequence.lightDue(123+86400000,1440));sequence.startLight(100);
    assert(!sequence.deepDue(100+86399999,1440) && sequence.deepDue(100+86400000,1440));
    sequence.screenOff(456);assert(!sequence.lightStarted());
    puts("PASS: shared battery sequence, exact staged deadlines, disabled timers, USB/unknown/dock guards, busy work, failed entry, wake-only keys, deep fallback, midnight-independent plan and rollover");
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

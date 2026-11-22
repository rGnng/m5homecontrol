#!/usr/bin/env python3
"""Run actual keypad IRQ and cached backlight scheduling code against hardware stubs."""
import ast
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/main.cpp').read_text()
module = ast.parse((ROOT / 'tools/test_controls.py').read_text())
arduino = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'arduino' for t in n.targets))

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

prefix = r'''
#include "app_config.h"
#include "backlight_schedule.h"
#include "keypad_controls.h"
#include <cassert>
#include <cstdio>
#include <ctime>
#define IRAM_ATTR
static uint32_t now=0;
static uint32_t millis(){return now;}
static bool idle=false;
static bool connectedPowerIdle(){return idle;}
static void setConnectedPowerIdle(bool value){idle=value;}
static void setPowerPollingIdle(bool){}
static constexpr int LOW=0,HIGH=1,INPUT=0,FALLING=2;
static int irqPin=HIGH,pinModes=0;
static void (*interruptHandler)()=nullptr;
static void pinMode(int pin,int mode){assert(pin==35 && mode==INPUT);++pinModes;}
static int digitalPinToInterrupt(int pin){return pin;}
static void attachInterrupt(int pin,void(*handler)(),int mode){assert(pin==35 && mode==FALLING);interruptHandler=handler;}
static int digitalRead(int pin){assert(pin==35);return irqPin;}
namespace m5 {enum class board_t {board_M5Stack,other};}
static m5::board_t board=m5::board_t::board_M5Stack;
static uint8_t rawKey=0;
static bool busOk=true,present=true,releaseOnRead=true,interruptInRead=false;
static int busReads=0;
struct Bus {
    bool scanID(uint8_t address,int hz){assert(address==0x08 && hz==100000);return present;}
    bool start(int address,bool read,int hz){assert(address==0x08 && read && hz==100000);++busReads;return busOk;}
    bool read(uint8_t* value,int count,bool last){
        assert(count==1 && last);*value=rawKey;rawKey=0;
        if(releaseOnRead)irqPin=HIGH;
        if(interruptInRead){interruptInRead=false;rawKey='5';irqPin=LOW;interruptHandler();}
        return busOk;
    }
    bool stop(){return busOk;}
};
struct {Bus In_I2C;m5::board_t getBoard(){return board;}}M5;
struct {template<class... T>void printf(const char*,T...){} }Serial;
'''
backlight_prefix = r'''
AppConfig g_config;
static bool clockAvailable=true;
static int clockHour=12,clockMinute=0,timeConversions=0,applies=0;
static bool haClockReady(){return clockAvailable;}
static bool haGetLocalTime(struct tm& value){++timeConversions;value={};value.tm_hour=clockHour;value.tm_min=clockMinute;return clockAvailable;}
static uint32_t g_lastActivity=0;
static int g_backlightOverride=-1;
static BacklightPhase g_backlightPhase=BacklightPhase::Normal;
static uint8_t g_backlightPercent=30;
static void applyBacklightPlan(BacklightPlan plan){++applies;g_backlightPhase=plan.phase;g_backlightPercent=plan.brightness;}
static void scheduleTick(bool explicitInput=false);
'''
tests = r'''
static void resetKeypad(){
    now=0;idle=false;irqPin=HIGH;pinModes=busReads=0;interruptHandler=nullptr;
    busOk=present=releaseOnRead=true;interruptInRead=false;rawKey=0;
    board=m5::board_t::board_M5Stack;
    g_keypadIrqAvailable=g_keypadIrqVerified=g_keypadIrqFault=g_keypadIrqPending=false;
    g_keypadReadCount=g_keypadIrqReads=g_keypadIrqFaults=0;
    g_lastKeyPoll=g_lastKeyTime=0;g_lastKey=0;
}
static void key(uint8_t raw,bool signal=true){rawKey=raw;if(signal){irqPin=LOW;if(interruptHandler)interruptHandler();}}
static void resetSchedule(uint32_t base=0){
    now=base;g_config=AppConfig{};g_config.dayBrightness=20;g_config.dayDimBrightness=3;
    g_config.nightBrightness=10;g_config.nightDimBrightness=1;
    g_lastActivity=base;g_backlightOverride=-1;g_backlightPhase=BacklightPhase::Normal;g_backlightPercent=30;
    clockAvailable=true;clockHour=12;clockMinute=0;timeConversions=applies=0;
    g_scheduleStarted=g_scheduleObservedClock=g_scheduleClockReady=g_scheduleNight=false;
    g_scheduleClockCheckedAt=g_nextBacklightCheck=g_backlightChecks=g_backlightClockChecks=0;
}
int main(){
    resetKeypad();keypadInit();assert(pinModes==1 && interruptHandler && !g_keypadIrqVerified);
    // No verified IRQ: preserve legacy active/idle polling and remapping.
    now=19;assert(!keypadPoll() && busReads==0);
    now=20;key('9',false);assert(keypadPoll()=='3' && !g_keypadIrqVerified);
    idle=true;now=119;assert(!keypadPoll() && busReads==1);
    now=120;key('8',false);assert(keypadPoll()=='2' && busReads==2);
    // A low/read/high key cycle proves wiring. Subsequent IRQs are read promptly.
    now=140;key('7');assert(keypadPoll()=='1' && g_keypadIrqVerified);
    assert(keypadPollIntervalMs()==1000);
    idle=false;assert(keypadPollIntervalMs()==20);idle=true;
    int reads=busReads;
    for(now=160;now<1140;now+=20)assert(!keypadPoll());
    assert(busReads==reads);
    now=1140;assert(!keypadPoll() && busReads==reads+1 && g_keypadIrqVerified);
    now=1160;key('4');assert(keypadPoll()=='4' && g_keypadIrqVerified);
    assert(keypadStatus().find("keypad_irq_verified=1")!=String::npos);
    // Retain an IRQ that arrives during the I2C read, then consume it next tick.
    now=1180;key('6');interruptInRead=true;assert(keypadPoll()=='6' && g_keypadIrqPending);
    now=1200;assert(keypadPoll()=='5' && !g_keypadIrqPending);
    // A key found only by the health poll demotes broken/missing IRQ wiring.
    now=2200;key('7',false);assert(keypadPoll()=='1' && !g_keypadIrqVerified);
    assert(g_keypadIrqFaults==1 && keypadPollIntervalMs()==100);
    // Stuck-low or failed I2C returns to normal polling, without a busy read loop.
    resetKeypad();keypadInit();idle=true;irqPin=LOW;releaseOnRead=false;
    now=20;assert(!keypadPoll() && g_keypadIrqFault && g_keypadIrqFaults==1);
    now=40;assert(!keypadPoll() && busReads==1);
    now=120;key('9');assert(keypadPoll()=='3' && !g_keypadIrqVerified);
    releaseOnRead=true;now=220;key('8');assert(keypadPoll()=='2' && g_keypadIrqVerified);
    now=240;key('7');busOk=false;assert(!keypadPoll() && !g_keypadIrqVerified);
    busOk=true;now=340;assert(keypadPoll()=='1' && g_keypadIrqVerified);
    // Pending data at boot, spurious edges, duplicate debounce and millis wrap.
    resetKeypad();key('9');keypadInit();now=20;assert(keypadPoll()=='3' && g_keypadIrqVerified);
    now=40;key('9');assert(!keypadPoll());
    now=60;interruptHandler();assert(!keypadPoll() && g_keypadIrqVerified);
    g_lastKeyPoll=UINT32_MAX-5;now=14;key('8');assert(keypadPoll()=='2');
    resetKeypad();board=m5::board_t::other;keypadInit();assert(!interruptHandler && pinModes==0);
    now=20;key('7',false);assert(keypadPoll()=='1');
    resetKeypad();present=false;keypadInit();assert(!interruptHandler && pinModes==0);
    assert(keypadStatus().find("keypad_irq_pin=-1")!=String::npos);
    std::puts("PASS: actual keypad IRQ verification, reduced reads, prompt keys, legacy/missing/stuck wiring, I2C failures, ISR races, debounce and rollover");

    resetSchedule();scheduleTick();assert(timeConversions==1 && applies==1 && g_backlightPercent==20);
    for(now=20;now<60000;now+=20)scheduleTick();
    assert(timeConversions==1 && applies==1);
    scheduleTick();assert(timeConversions==2 && applies==2);
    // Idle deadlines remain exact while all intervening loops skip clock work.
    resetSchedule();g_config.dayDimSeconds=2;g_config.dayOffSeconds=3;scheduleTick();
    assert(g_nextBacklightCheck==2000);
    now=1999;scheduleTick();assert(g_backlightPhase==BacklightPhase::Normal && applies==1);
    now=2000;scheduleTick();assert(g_backlightPhase==BacklightPhase::Dim && g_backlightPercent==3);
    assert(timeConversions==1 && g_nextBacklightCheck==5000);
    now=4999;scheduleTick();assert(applies==2);
    now=5000;scheduleTick();assert(g_backlightPhase==BacklightPhase::Off && timeConversions==1);
    now=5001;noteActivity();assert(g_backlightPhase==BacklightPhase::Normal && g_lastActivity==5001);
    assert(g_nextBacklightCheck==7001 && timeConversions==1);
    assert(setDisplayMode("off") && g_backlightPhase==BacklightPhase::Off);
    assert(setDisplayMode("wake") && g_backlightPhase==BacklightPhase::Normal);
    assert(setDisplayMode("dim") && g_backlightPhase==BacklightPhase::Dim);
    assert(!setDisplayMode("invalid"));
    assert(timeConversions==1);
    resetSchedule();scheduleTick();
    for(now=100;now<60000;now+=100)noteActivity();
    assert(timeConversions==1 && g_backlightClockChecks==1 && applies==600);
    scheduleTick();assert(timeConversions==2);
    // First HA time is applied immediately; subsequent day/night checks are minute-spaced.
    resetSchedule();clockAvailable=false;scheduleTick();assert(g_backlightPercent==30);
    now=10;clockAvailable=true;scheduleTick();assert(g_backlightPercent==20 && timeConversions==2);
    resetSchedule();clockHour=20;clockMinute=59;scheduleTick();
    now=59999;clockHour=21;clockMinute=0;scheduleTick();assert(g_backlightPercent==20);
    now=60000;scheduleTick();assert(g_backlightPercent==10 && timeConversions==2);
    clockHour=12;now=120000;scheduleTick();assert(g_backlightPercent==20);
    noteActivity();assert(g_backlightPercent==20);
    // Disabled timers and rollover never cause a busy loop or premature dim.
    resetSchedule(UINT32_MAX-1000);g_config.dayDimSeconds=2;g_config.dayOffSeconds=3;
    scheduleTick();now=998;scheduleTick();assert(applies==1);
    now=999;scheduleTick();assert(g_backlightPhase==BacklightPhase::Dim && timeConversions==1);
    now=3999;scheduleTick();assert(g_backlightPhase==BacklightPhase::Off && timeConversions==1);
    resetSchedule(UINT32_MAX-1000);scheduleTick();now=58998;scheduleTick();assert(applies==1);
    now=58999;scheduleTick();assert(applies==2 && timeConversions==2);
    std::puts("PASS: actual minute clock cache, skipped background work, exact idle deadlines, immediate input/web wake, first time sync, day/night, disabled timers and rollover");
}
'''
keypad = source[source.index('static constexpr int KEYPAD_IRQ_PIN'):source.index('// The LED task')]
schedule_globals = source[source.index('static constexpr uint32_t BACKLIGHT_SCHEDULE_INTERVAL_MS'):source.index('static void applyBacklightPercent(')]
with tempfile.TemporaryDirectory(prefix='m5-idle-io-') as directory:
    tmp = Path(directory)
    (tmp/'Arduino.h').write_text(arduino)
    code = prefix + keypad + backlight_prefix + schedule_globals
    code += ''.join(function(s) for s in ['static void noteActivity(', 'static bool inTimeRange(',
                                         'static void scheduleTick(bool explicitInput) {', 'bool setDisplayMode('])
    (tmp/'test.cpp').write_text(code + tests)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{tmp}',f'-I{ROOT/"include"}',
                    str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)

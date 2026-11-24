#!/usr/bin/env python3
"""Exercise the actual GPIO26 handlers with simulated contacts and UI state."""
import ast
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
module = ast.parse((ROOT/'tools/test_controls.py').read_text())
arduino = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'arduino' for t in n.targets))
source = (ROOT/'src/main.cpp').read_text()
def function(signature):
    start = source.index(signature)
    pos = source.index('{', start)
    end, depth = pos+1, 1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end += 1
    return source[start:end]+'\n'

prefix = r'''
#include "dock_input.h"
#include "deep_sleep.h"
#include "sleep_sequence.h"
#include "backlight_schedule.h"
#include <cassert>
#include <cstdio>
namespace m5{enum class board_t{board_M5Stack,other};}
struct{m5::board_t board=m5::board_t::board_M5Stack;m5::board_t getBoard(){return board;}}M5;
constexpr int LOW=0,HIGH=1,INPUT_PULLUP=2;
static uint32_t now=0,g_lastActivity=0,g_dimStartedAt=0;
static int level=1,pinModes=0,displayChanges=0,animations=0;
static bool networkOff=false;
static bool g_statusDockDirty=false;
static BacklightPhase g_backlightPhase=BacklightPhase::Normal;
static uint8_t g_backlightPercent=1;
static DeepWake cause=DeepWake::None;
static int previousLevel=1;
uint32_t millis(){return now;}
static void pinMode(int pin,int mode){assert(pin==26 && mode==INPUT_PULLUP);++pinModes;}
static int digitalRead(int pin){assert(pin==26);return level;}
DeepWake deepSleepWakeCause(){return cause;}
int deepSleepDockLevel(){return previousLevel;}
struct{template<class... Args>void printf(const char*,Args...){}}Serial;
static void animateDockLeds(){++animations;}
static void noteActivity();
static void applyBacklightPlan(BacklightPlan plan){g_backlightPhase=plan.phase;}
bool networkSetSuspended(bool suspended){networkOff=suspended;return true;}
bool setDisplayMode(const String& mode){
    assert(mode=="dim" || mode=="off");++displayChanges;
    g_backlightPhase=mode=="dim"?BacklightPhase::Dim:BacklightPhase::Off;return true;
}
'''
start = source.index('static constexpr int DOCK_INPUT_PIN')
end = source.index('static uint32_t g_lastActivity', start)
tests = r'''
int main(){
    dockInputInit();assert(pinModes==1 && !dockInputPending() && dockInputLevel()==1);
    now=100;level=0;assert(dockInputPending());dockInputTick();
    assert(g_backlightPhase==BacklightPhase::Normal && dockInputPending());
    now=150;dockInputTick();
    assert(g_backlightPhase==BacklightPhase::Normal && !dockInputPending() && dockInputLevel()==0);
    assert(g_lastActivity==0 && g_dimStartedAt==0 && g_statusDockDirty && dockConnected());
    now=1000;dockInputTick();assert(displayChanges==0);
    g_backlightPhase=BacklightPhase::Dim;
    level=1;dockInputTick();now=1050;dockInputTick();
    assert(g_backlightPhase==BacklightPhase::Normal && dockInputLevel()==1 && !dockConnected());
    // A light-sleep dock wake stays Off through debounce, then goes directly to Dim.
    g_backlightPhase=BacklightPhase::Off;networkOff=true;g_resumeDeepSleep=true;
    now=2000;level=0;dockInputTick();assert(g_backlightPhase==BacklightPhase::Off);
    now=2050;dockInputTick();
    assert(g_backlightPhase==BacklightPhase::Dim && !networkOff && !g_resumeDeepSleep);
    // Reconstruct the retained contact level after an RTC docking wake.
    g_backlightPhase=BacklightPhase::Off;networkOff=true;g_resumeDeepSleep=true;
    cause=DeepWake::DockChange;previousLevel=1;level=0;now=3000;dockInputInit();
    assert(!g_dockInput.docked() && dockInputPending());dockInputTick();
    now=3050;dockInputTick();assert(g_backlightPhase==BacklightPhase::Dim && !networkOff);
    // Pickup wakes the normal view without playing another sound.
    previousLevel=0;level=1;now=4000;dockInputInit();
    g_backlightPhase=BacklightPhase::Off;networkOff=true;g_resumeDeepSleep=true;
    dockInputTick();now=4050;dockInputTick();
    assert(!g_dockInput.docked() && g_backlightPhase==BacklightPhase::Normal && !networkOff && !g_resumeDeepSleep);
    assert(dockInputStatus().find("dock_detected=0")!=String::npos);
    // Ordinary boot with an existing short is not a new docking event.
    cause=DeepWake::None;level=0;now=5000;dockInputInit();dockInputTick();
    now=5050;dockInputTick();assert(animations==3);
    level=1;now=5100;dockInputTick();now=5150;dockInputTick();
    g_backlightPhase=BacklightPhase::Dim;g_lastActivity=333;g_dimStartedAt=444;
    level=0;now=5200;dockInputTick();now=5250;dockInputTick();
    assert(animations==4 && g_backlightPhase==BacklightPhase::Dim);
    assert(g_lastActivity==333 && g_dimStartedAt==444 && displayChanges==2);
    M5.board=m5::board_t::other;dockInputInit();assert(dockInputLevel()==-1 && !dockInputPending());
    puts("PASS: awake modes/timers preserved, sleeping dock wakes Dim, pickup wakes Normal, silent dock animation and RTC baseline");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-dock-wake-') as directory:
    tmp=Path(directory)
    (tmp/'Arduino.h').write_text(arduino)
    activity='static void noteActivity(){networkOff=false;g_backlightPhase=BacklightPhase::Normal;g_lastActivity=now;g_resumeDeepSleep=false;g_dockDimTimeout=false;}\n'
    (tmp/'test.cpp').write_text(prefix+source[start:end]+activity+
        function('static void onDocked() {')+function('static void onUndocked() {')+tests)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{tmp}',f'-I{ROOT/"include"}',
                    str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)

#!/usr/bin/env python3
"""Run the actual LED task against a timed notification queue and ten-pixel strip."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'src/main.cpp').read_text()
start = source.index('static constexpr uint32_t COMMAND_LED_DURATION_MS')
end = source.index('static void initCommandLeds()',start)
prefix = r'''
#include "dock_led_animation.h"
#include "low_battery_warning.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
using TaskHandle_t=void*;
constexpr uint32_t portMAX_DELAY=UINT32_MAX;
constexpr int pdTRUE=1;
static uint32_t now=0;
uint32_t millis(){return now;}
uint32_t pdMS_TO_TICKS(uint32_t ms){return ms;}
template<class T>T constrain(T v,T lo,T hi){return std::max(lo,std::min(hi,v));}
struct Done{};
struct Notice{uint32_t at,value;};
static std::vector<Notice> notices;
int xTaskNotifyWait(uint32_t,uint32_t,uint32_t* out,uint32_t wait){
    if(!notices.empty() && (wait==portMAX_DELAY || notices.front().at<=now+wait)){
        auto n=notices.front();notices.erase(notices.begin());now=n.at;*out=n.value;return pdTRUE;
    }
    if(wait==portMAX_DELAY)throw Done{};
    now+=wait;*out=0;return 0; // Timeout output is deliberately overwritten.
}
struct Frame{uint32_t at;std::array<uint32_t,10> pixels;};
static std::vector<Frame> frames;
struct Strip{
    std::array<uint32_t,10> pixels{};
    void setAllColor(uint8_t r,uint8_t g,uint8_t b){pixels.fill((r<<16)|(g<<8)|b);}
    void setColor(int i,uint8_t r,uint8_t g,uint8_t b){assert(i>=0 && i<10);pixels[i]=(r<<16)|(g<<8)|b;}
    void display(){frames.push_back({now,pixels});}
};
struct{Strip Led;}M5;
'''
tests = r'''
static void run(std::vector<Notice> input){
    notices=input;now=0;frames.clear();
    try{commandLedTask(nullptr);assert(false);}catch(const Done&){}
    assert((!g_commandLedActive && frames.back().pixels==std::array<uint32_t,10>{}));
}
int main(){
    assert(!g_commandLedTask && g_commandLedCount==0 && g_dockLedCount==0);
    g_commandLedBrightness=0;run({{0,DOCK_LED_EVENT}});
    uint32_t priorPeak=0;
    bool redPhase=false,greenPhase=false;
    for(const auto& frame:frames){
        for(auto p:frame.pixels)assert(p==frame.pixels[0]);
        auto p=frame.pixels[0];auto red=p>>16,green=(p>>8)&255;
        auto peak=std::max(red,green);
        assert(peak<=38 && (p&255)==0);
        if(frame.at<=70)assert(peak>=priorPeak);
        else if(frame.at<=280)assert(peak==38);
        else assert(peak<=priorPeak);
        if(frame.at<175 && red>green)redPhase=true;
        if(frame.at>175 && green>red)greenPhase=true;
        priorPeak=peak;
    }
    assert(frames.front().pixels[0]==0 && now==350 && priorPeak==0 && redPhase && greenPhase);
    assert(g_dockLedFrames>=35 && g_dockLedPixels==1023);
    // HA requests arriving during docking wait until its 350 ms finish.
    g_commandLedBrightness=255;run({{0,DOCK_LED_EVENT},{30,0xff0000}});
    bool command=false;
    for(const auto& frame:frames){
        if(frame.at<350)for(auto p:frame.pixels){
            assert(p==frame.pixels[0] && (p>>16)<=38 && ((p>>8)&255)<=38 && (p&255)==0);
        }
        for(auto p:frame.pixels)if(frame.at>=350 && p && !(p&0xffff))command=true;
    }
    assert(command && g_commandLedFrames>0 && g_commandLedPixels==1023 && now==650);
    // A new docking event can preempt a normal sweep and light every pixel.
    run({{0,0x00ff00},{40,DOCK_LED_EVENT}});
    bool darkStart=false,red=false;
    for(const auto& frame:frames){
        if(frame.at==40 && frame.pixels[0]==0){
            darkStart=true;for(auto p:frame.pixels)assert(p==0);
        }
        if(frame.at==50 && frame.pixels[0]>0){
            red=true;for(auto p:frame.pixels)assert(p==frame.pixels[0] && !(p&0xffff));
        }
    }
    assert(darkStart && red && now==390 && g_dockLedPixels==1023);
    // Low-battery warnings use both strips, even with the display brightness at zero.
    g_lowBatteryLedAllowed=true;g_commandLedBrightness=0;
    run({{0,LOW_BATTERY_LED_EVENT}});
    for(const auto& frame:frames){
        auto expected=(frame.at<80 || (frame.at>=160 && frame.at<240))?0x260000u:0u;
        for(auto p:frame.pixels)assert(p==expected);
    }
    assert(now==240);
    // Docking or disabling an already-queued warning suppresses it completely.
    g_lowBatteryLedAllowed=false;run({{0,LOW_BATTERY_LED_EVENT}});
    assert(now==0 && frames.size()==1);
    // User feedback can interrupt a warning; an extra warning never interrupts docking.
    g_lowBatteryLedAllowed=true;g_commandLedBrightness=255;
    run({{0,LOW_BATTERY_LED_EVENT},{30,DOCK_LED_EVENT}});
    assert(now==380 && g_dockLedPixels==1023);
    run({{0,DOCK_LED_EVENT},{30,LOW_BATTERY_LED_EVENT}});
    assert(now==350 && g_dockLedPixels==1023);
    run({{0,LOW_BATTERY_LED_EVENT},{30,0x0000ff}});
    assert(now==330 && g_commandLedPixels==1023);
    puts("PASS: real ten-LED task, docking fade, low-battery double flash, suppression, exact duration and feedback priority");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-dock-leds-') as directory:
    tmp=Path(directory);(tmp/'test.cpp').write_text(prefix+source[start:end]+tests)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{ROOT/"include"}',
                    str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)

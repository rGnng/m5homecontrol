#!/usr/bin/env python3
"""Check real RMT encoding plus bounded completion and recovery after driver faults."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
HEADERS={
    'Arduino.h':r'''
#pragma once
#include <cstdint>
#include <cstddef>
inline uint32_t now=0;
inline uint32_t millis(){return now;}
constexpr int OUTPUT=1,LOW=0;
inline void pinMode(int,int){}inline void digitalWrite(int,int){}
struct Console{void println(const char*){}};inline Console Serial;
''',
    'utility/led/LED_Strip_Class.hpp':r'''
#pragma once
#include <cstdint>
#include <cstddef>
#define M5UNIFIED_RMT_VERSION 1
namespace m5{class LedBus_Base{public:virtual ~LedBus_Base()=default;virtual bool init()=0;virtual void release()=0;virtual void write(const uint8_t*,size_t)=0;};}
''',
    'driver/rmt.h':r'''
#pragma once
#include <Arduino.h>
#include <vector>
#include <cassert>
using gpio_num_t=int;using rmt_channel_t=int;
constexpr int GPIO_NUM_MAX=40,RMT_CHANNEL_MAX=8,RMT_CHANNEL_UNINIT=0,ESP_OK=0,ESP_INTR_FLAG_IRAM=1;
inline bool GPIO_IS_VALID_OUTPUT_GPIO(int p){return p>=0 && p<34;}
inline unsigned pdMS_TO_TICKS(unsigned ms){return ms;}
struct rmt_config_t{int gpio_num,channel,clk_div;};
#define RMT_DEFAULT_CONFIG_TX(pin,ch) rmt_config_t{pin,ch,80}
struct rmt_item32_t{union{struct{uint32_t duration0:15,level0:1,duration1:15,level1:1;};uint32_t val;};};
struct rmt_channel_status_result_t{int status[8];};
inline int installed[8]{},installs=0,uninstalls=0,writes=0,stops=0;
inline bool failInstall=false,failWrite=false,failWait=false;
inline unsigned waitMs=0;inline std::vector<rmt_item32_t> lastFrame;
inline int rmt_get_channel_status(rmt_channel_status_result_t* s){for(int i=0;i<8;++i)s->status[i]=installed[i];return 0;}
inline int rmt_config(rmt_config_t* c){assert(c->gpio_num==15 && c->clk_div==2);return 0;}
inline int rmt_driver_install(int c,int,int flags){assert(flags==ESP_INTR_FLAG_IRAM);++installs;if(failInstall)return -1;assert(!installed[c]);installed[c]=1;return 0;}
inline int rmt_tx_stop(int c){assert(installed[c]);++stops;return 0;}
inline int rmt_driver_uninstall(int c){assert(installed[c]);installed[c]=0;++uninstalls;return 0;}
inline int rmt_write_items(int c,const rmt_item32_t* items,int n,bool wait){assert(installed[c] && !wait);++writes;lastFrame.assign(items,items+n);return failWrite?-1:0;}
inline int rmt_wait_tx_done(int c,unsigned ms){assert(installed[c]);waitMs=ms;if(failWait){now+=ms;return -1;}return 0;}
''',
}
TEST=r'''
#include "side_led_bus.h"
#include <cstdio>
int main(){
    installed[0]=1; // Another peripheral owns channel 0; leave it alone.
    SideLedBus bus(15);assert(bus.init() && installed[1]);
    uint8_t pixels[30]{};pixels[0]=0x80;bus.write(pixels,30);
    assert(writes==1 && lastFrame.size()==241 && waitMs==20);
    for(size_t i=0;i<240;++i){auto bit=lastFrame[i];
        assert(bit.level0==1 && bit.level1==0);
        assert(bit.duration0==(i==0?24:12) && bit.duration1==(i==0?24:36));
    }
    assert(lastFrame.back().val && lastFrame.back().level0==0 && lastFrame.back().level1==0);
    assert(lastFrame.back().duration0+lastFrame.back().duration1==3200);
    failWait=true;bus.write(pixels,30);assert(now==20 && uninstalls==1 && stops==1 && !installed[1]);
    bus.write(pixels,30);assert(writes==2);now=1019;bus.write(pixels,30);assert(writes==2);
    now=1020;failWait=false;bus.write(pixels,30);assert(writes==3 && installed[1]);
    failWrite=true;bus.write(pixels,30);assert(uninstalls==2 && !installed[1]);
    now=2020;failWrite=false;bus.write(pixels,30);assert(writes==5 && installed[1]);
    bus.write(nullptr,30);bus.write(pixels,0);assert(writes==5);
    bus.release();bus.release();assert(uninstalls==3 && installed[0]==1);
    failInstall=true;SideLedBus retry(15);assert(!retry.init());failInstall=false;assert(retry.init());retry.release();
    SideLedBus invalid(39);assert(!invalid.init());
    puts("PASS: all 240 LED bits, latch timing, channel ownership, 20 ms failure limit, cooldown, reinitialization and cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-side-led-') as directory:
    tmp=Path(directory)
    for name,content in HEADERS.items():
        path=tmp/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content)
    (tmp/'test.cpp').write_text(TEST)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{tmp}',f'-I{ROOT/"include"}',str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True,timeout=5)

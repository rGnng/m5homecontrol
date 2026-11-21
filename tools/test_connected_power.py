#!/usr/bin/env python3
"""Exercise actual CPU guards, idle hardware sequencing, and REST refresh recovery."""
import ast
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
main = (ROOT / 'src/main.cpp').read_text()
module = ast.parse((ROOT / 'tools/test_controls.py').read_text())
arduino = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'arduino' for t in n.targets))

def function(signature):
    start = main.index(signature)
    pos = main.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (main[end] == '{') - (main[end] == '}')
        end += 1
    return main[start:end] + '\n'

cpu_stubs = r'''
#pragma once
#include <cstdint>
#include <vector>
inline uint32_t cpu=240;
inline bool cpuAllowed=true;
inline std::vector<uint32_t> frequencies;
inline uint32_t getCpuFrequencyMhz(){return cpu;}
inline bool setCpuFrequencyMhz(uint32_t value){frequencies.push_back(value);if(!cpuAllowed)return false;cpu=value;return true;}
struct Logger {template<class... T>void printf(const char*,T...){} };
inline Logger Serial;
'''
wifi_stubs = r'''
#pragma once
enum wifi_ps_type_t {WIFI_PS_NONE,WIFI_PS_MIN_MODEM,WIFI_PS_MAX_MODEM};
constexpr int ESP_OK=0;
inline wifi_ps_type_t ps=WIFI_PS_MIN_MODEM;
inline int psError=0;
inline int esp_wifi_get_ps(wifi_ps_type_t* value){*value=ps;return psError;}
inline int esp_wifi_set_ps(wifi_ps_type_t value){if(psError)return psError;ps=value;return ESP_OK;}
'''
cpu_test = r'''
#include "connected_power.h"
#include <esp32-hal-cpu.h>
#include <esp_wifi.h>
#include <cassert>
int main(){
    setConnectedPowerIdle(false);assert(cpu==160 && frequencies==std::vector<uint32_t>({160}));
    setConnectedPowerIdle(true);assert(cpu==80 && connectedPowerIdle());
    {
        ConnectedPowerWork outer;assert(cpu==160 && ps==WIFI_PS_NONE);
        {ConnectedPowerWork inner;assert(cpu==160 && ps==WIFI_PS_NONE);}
        assert(cpu==160 && ps==WIFI_PS_NONE);
        setConnectedPowerIdle(true);assert(cpu==160);
    }
    assert(cpu==80 && ps==WIFI_PS_MIN_MODEM);
    endConnectedPowerWork();assert(cpu==80);
    setConnectedPowerIdle(false);assert(cpu==160);
    cpuAllowed=false;setConnectedPowerIdle(true);assert(cpu==160);
    cpuAllowed=true;setConnectedPowerIdle(true);assert(cpu==80);
    assert(connectedPowerStatus().find("cpu_mhz=80 wifi_sleep=minimum")!=String::npos);
    ps=WIFI_PS_NONE;assert(connectedPowerStatus().find("wifi_sleep=none")!=String::npos);
    psError=1;assert(connectedPowerStatus().find("wifi_sleep=unavailable")!=String::npos);
    std::puts("PASS: actual CPU idle/boost, nested work guards, failed frequency changes and Wi-Fi diagnostics");
}
'''
deadline_test = r'''
#include <Arduino.h>
#include <cassert>
static uint32_t now=0;
static uint32_t millis(){return now;}
static void delay(int ms){now+=ms;}
#include "deadline_stream.h"
struct Client {
    bool online=true,trickle=false;
    String data="abc";size_t position=0;uint32_t nextAt=0;
    int read(){
        if(trickle){if(now<nextAt)return -1;nextAt=now+10;return 'x';}
        return position<data.size()?static_cast<unsigned char>(data[position++]):-1;
    }
    bool connected(){return online;}
};
int main(){
    Client client;client.online=false;DeadlineStream<Client> complete(client,25);char out[10]={};
    assert(complete.readBytes(out,10)==3 && String(out)=="abc" && now==0);
    client=Client{};client.data="";DeadlineStream<Client> stalled(client,25);
    assert(stalled.read()==-1 && now==25);
    client.trickle=true;client.nextAt=now;DeadlineStream<Client> trickle(client,25);
    assert(trickle.readBytes(out,10)==3 && now==50);
    now=UINT32_MAX-10;client.trickle=false;DeadlineStream<Client> rollover(client,25);
    assert(rollover.read()==-1 && now==14);
    std::puts("PASS: actual JSON reader deadline for EOF, stalled and trickling servers, and millis rollover");
}
'''
policy_prefix = r'''
#include "app_config.h"
#include "ha_client.h"
#include "backlight_schedule.h"
#include <cassert>
#include <vector>
#include <algorithm>
static uint32_t now=0;
static uint32_t millis(){return now;}
static bool idle=false,pollIdle=false;
static void setConnectedPowerIdle(bool value){idle=value;}
static void setPowerPollingIdle(bool value){pollIdle=value;}
namespace m5 {enum class board_t{board_M5Stack};}
static constexpr int OUTPUT=1,LOW=0;
static int dacOff=0,pinWrites=0;
static void dacDisable(int pin){assert(pin==25);++dacOff;}
static void pinMode(int pin,int mode){assert(pin==25 && mode==OUTPUT);}
static void digitalWrite(int pin,int value){assert(pin==25 && value==LOW);++pinWrites;}
static std::vector<String> panelEvents;
struct Speaker {
    bool playing=false;int ends=0;
    void end(){++ends;}
    bool isPlaying(){return playing;}
    bool playWav(const uint8_t*,size_t,int,int,bool){playing=true;return true;}
};
struct Panel {
    void startWrite(){}
    void endWrite(){}
    void sleep(){panelEvents.push_back("sleep");}
    void wakeup(){panelEvents.push_back("wake");}
};
struct {struct Speaker Speaker;Panel Display;m5::board_t getBoard(){return m5::board_t::board_M5Stack;}}M5;
static uint32_t g_lastSoundAt=0,g_lastActivity=0;
static bool g_audioQuiet=false,g_lcdSleeping=false,g_dimTheme=false;
static BacklightPhase g_backlightPhase=BacklightPhase::Normal;
static void applyBacklightPercent(uint8_t value){panelEvents.push_back("brightness="+String(value));}
static void applyTheme(){panelEvents.push_back("theme");}
static void renderInitial(){panelEvents.push_back("paint");}
AppConfig g_config;
static constexpr int WL_CONNECTED=3;
static int wifiStatus=WL_CONNECTED;
struct {int status(){return wifiStatus;}}WiFi;
static bool configured=true,wsReady=true,fetchOk=true,auxOk=true,injectAux=false;
bool haConfigured(){return configured;}
bool haStateUpdatesReady(){return wsReady;}
static EntityState g_cards[MAX_ENTITIES];
static bool g_dirty[MAX_ENTITIES]={};
static int g_focus=0,draws=0;
static int globalSlot(int slot){return slot;}
static void drawMenuRow(Panel&,int,const EntityState&,bool,bool){++draws;}
static void redrawAcDetail(){++draws;}
static void redrawStatusRegions(){}
static int g_page=0,g_detailGlobal=-1,g_refreshIndex=-1,g_refreshFail=0;
static bool g_detail=false;
static uint32_t g_nextRefresh=0,g_lastFullRefresh=0,g_restRefreshReads=0,g_fullRefreshes=0;
static bool g_refreshPending[MAX_ENTITIES]={},g_fullRefreshStarted=false;
static std::vector<String> reads;
bool haFetchState(EntityState& e){
    reads.push_back(e.id);e.state="rest";
    if(injectAux && e.id=="switch.aux"){g_cards[0].aux.state="live";++g_cards[0].aux.revision;}
    return e.id=="switch.aux"?auxOk:fetchOk;
}
'''
policy_tests = r'''
static void sweep(){for(int i=0;i<MAX_ENTITIES+2;++i)refreshTick();}
int main(){
    // Audio never shuts down during playback; idle begins after input timeout.
    now=14999;connectedPowerTick();assert(!idle && g_audioQuiet && dacOff==1 && pinWrites==1);
    now=15000;connectedPowerTick();assert(idle && pollIdle);
    uint8_t sample=0;playUiSound(&sample,1);assert(!idle && !g_audioQuiet);
    now+=2000;connectedPowerTick();assert(!idle && !g_audioQuiet);
    M5.Speaker.playing=false;connectedPowerTick();assert(idle && g_audioQuiet && dacOff==2);
    g_lastActivity=now;g_backlightPhase=BacklightPhase::Dim;connectedPowerTick();assert(idle);
    g_backlightPhase=BacklightPhase::Normal;connectedPowerTick();assert(!idle);
    // Off sleeps the panel after brightness zero. Wake repaints in darkness.
    panelEvents.clear();applyBacklightPlan({BacklightPhase::Off,0});
    assert(g_lcdSleeping && panelEvents==std::vector<String>({"brightness=0","sleep"}));
    assert(beginDisplayRead());endDisplayRead(true);
    assert(g_lcdSleeping && panelEvents[2]=="wake" && panelEvents[3]=="sleep");
    panelEvents.clear();applyBacklightPlan({BacklightPhase::Normal,5});
    assert(!g_lcdSleeping && panelEvents==std::vector<String>({"wake","brightness=0","theme","paint","brightness=5"}));
    panelEvents.clear();assert(!beginDisplayRead());endDisplayRead(false);assert(panelEvents.empty());
    applyBacklightPlan({BacklightPhase::Dim,1});assert(g_dimTheme && !g_lcdSleeping);
    // Initial sweep includes all configured slots and AUX values, then remains quiet.
    now=20000;g_config.entityCount=4;
    g_cards[0].id="light.a";g_cards[0].aux.id="switch.aux";
    g_cards[1].id="climate.ac";g_cards[2].id="light.a";
    sweep();assert(reads==std::vector<String>({"light.a","switch.aux","climate.ac","light.a"}));
    assert(g_fullRefreshes==1 && g_restRefreshReads==4);
    now+=300000;sweep();assert(reads.size()==4);
    requestEntityRefresh("climate.ac");sweep();assert(reads.size()==4);
    now+=1500;sweep();assert(reads.size()==5 && reads.back()=="climate.ac");
    requestEntityRefresh("light.a");now+=1500;sweep();assert(reads.size()==8);
    // AUX errors retry after 15s; a live update during REST wins.
    auxOk=false;requestEntityRefresh("switch.aux");now+=1500;sweep();
    assert(g_refreshPending[0] && g_nextRefresh==now+15000);
    size_t count=reads.size();now+=14999;sweep();assert(reads.size()==count);
    ++now;auxOk=true;injectAux=true;sweep();assert(g_cards[0].aux.state=="live" && !g_refreshPending[0]);injectAux=false;
    // Subscription loss falls back to 30s sweeps; no busy retry on HTTP errors.
    wsReady=false;fetchOk=false;now+=30000;sweep();
    assert(g_refreshPending[0] && g_refreshPending[1] && g_refreshPending[2]);
    count=reads.size();now+=14999;sweep();assert(reads.size()==count);
    ++now;fetchOk=true;sweep();assert(!g_refreshPending[0] && !g_refreshPending[1] && !g_refreshPending[2]);
    // Connection recovery forces a full snapshot. Offline never sends HTTP.
    wifiStatus=0;count=reads.size();now+=30000;sweep();assert(reads.size()==count);
    wifiStatus=WL_CONNECTED;onHaAuth();sweep();assert(reads.size()==count+4);
    g_lcdSleeping=true;g_dirty[0]=true;renderDirty();assert(g_dirty[0] && draws==0);
    g_lcdSleeping=false;renderDirty();assert(!g_dirty[0] && draws>0);
    // Duplicate entities stay consistent in every primary and AUX slot.
    g_cards[1].aux.id="light.a";g_cards[2].aux.id="light.a";g_cards[0].customName="Custom";
    EntityState state;state.id="light.a";state.state="on";state.revision=7;
    onHaState(state);assert(g_cards[0].state=="on" && g_cards[2].state=="on");
    assert(g_cards[2].aux.state=="on");
    assert(g_cards[1].aux.state=="on" && g_cards[0].name=="Custom");
    std::puts("PASS: actual idle/audio policy, dark LCD wake/read sequencing, targeted REST, AUX races/errors, retry backoff, offline/reconnect and duplicate states");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-connected-power-') as directory:
    tmp = Path(directory)
    (tmp/'Arduino.h').write_text(arduino)
    (tmp/'esp32-hal-cpu.h').write_text(cpu_stubs)
    (tmp/'esp_wifi.h').write_text(wifi_stubs)
    (tmp/'cpu.cpp').write_text(cpu_test)
    flags=['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{tmp}',f'-I{ROOT/"include"}']
    subprocess.run(flags+[str(ROOT/'src/connected_power.cpp'),str(tmp/'cpu.cpp'),'-o',str(tmp/'cpu')],check=True)
    subprocess.run([str(tmp/'cpu')],check=True)
    (tmp/'deadline.cpp').write_text(deadline_test)
    subprocess.run(flags+[str(tmp/'deadline.cpp'),'-o',str(tmp/'deadline')],check=True)
    subprocess.run([str(tmp/'deadline')],check=True)
    signatures=['static void quietAudio(', 'static void playUiSound(', 'static void connectedPowerTick(',
                'bool beginDisplayRead(', 'void endDisplayRead(', 'static void applyBacklightPlan(',
                'static void renderDirty(', 'static void requestEntityRefresh(', 'static void onHaAuth(', 'static void onHaState(', 'static void refreshTick(']
    (tmp/'policy.cpp').write_text(policy_prefix+''.join(function(s) for s in signatures)+policy_tests)
    subprocess.run(flags+[str(tmp/'policy.cpp'),'-o',str(tmp/'policy')],check=True)
    subprocess.run([str(tmp/'policy')],check=True)

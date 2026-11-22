#!/usr/bin/env python3
"""Exercise actual dim-input refresh/dispatch, including navigation and delayed replies."""
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
#include "ha_client.h"
#include "backlight_schedule.h"
#include "keypad_controls.h"
#include <cassert>
#include <vector>
#include <algorithm>
using std::isfinite;
using std::isnan;
template<class T>static T constrain(T v,T a,T b){return std::min(b,std::max(a,v));}
static uint32_t now=0,generation=0;
static uint32_t millis(){return now;}
static bool suspended=false,busy=false;
bool haDataSuspended(){return suspended;}
uint32_t haDataGeneration(){return generation;}
bool haHttpBusy(){return busy;}
AppConfig g_config;
static EntityState g_cards[MAX_ENTITIES];
static int g_page=0,g_focus=0,g_detailGlobal=0,g_acCursor=0;
static bool g_detail=false,g_acEditing=false;
static float g_acDraftTemp=NAN;
static constexpr int AC_ROWS=7;
static BacklightPhase g_backlightPhase=BacklightPhase::Normal;
static int globalSlot(int slot){return g_page*GRID_SLOTS+slot;}
static EntityState& cardAt(int slot){return g_cards[globalSlot(slot)];}
static EntityState& acEntity(){return g_cards[g_detailGlobal];}
static bool acDetailActive(){return g_detail && acEntity().type==EntityType::Climate;}
static EntityState& selectedEntity(){return g_detail?acEntity():cardAt(g_focus);}
bool setDisplayMode(const String& mode){
    assert(mode=="dim");if(!suspended){suspended=true;++generation;}
    g_backlightPhase=BacklightPhase::Dim;
    for(auto& e:g_cards){e.available=false;e.aux.available=false;}return true;
}
static void noteActivity(){
    if(suspended){suspended=false;++generation;}
    g_backlightPhase=BacklightPhase::Normal;
}
static HaResultCallback readComplete,commandComplete;
static EntityState* readDestination=nullptr;
static std::vector<String> reads;
struct Call {String service,id,extra;};
static std::vector<Call> calls;
bool haFetchState(EntityState& e,HaResultCallback complete){
    assert(!busy);busy=true;readDestination=&e;readComplete=complete;reads.push_back(e.id);return true;
}
static void finishRead(EntityState state,bool ok=true){
    assert(readDestination && readComplete);
    *readDestination=state;readDestination=nullptr;busy=false;
    auto complete=std::move(readComplete);readComplete=nullptr;complete(ok);
}
static bool callService(const String&,const String& service,const String& id,const String& extra="",HaResultCallback complete=nullptr){
    calls.push_back({service,id,extra});commandComplete=complete;return true;
}
static void finishCommand(){auto complete=std::move(commandComplete);commandComplete=nullptr;if(complete)complete(true);}
String entityDomain(const String& id){return id.substr(0,id.find('.'));}
static void scheduleRefresh(){}
static void sfxArrow(){}
static void sfxSelect(){}
static void redrawAcRow(int){}
static void redrawAcDetail(){}
static void redrawRow(int,bool){}
static void redrawStatusRegions(){}
static void drawMenuRow(int,int,const EntityState&,bool){}
struct Button {
    bool pressed=false,pressEdge=false,releaseEdge=false,longPress=false;
    bool isPressed(){return pressed;}
    bool wasPressed(){return pressEdge;}
    bool wasReleased(){return releaseEdge;}
    bool pressedFor(int){return longPress;}
};
struct Display {void startWrite(){} void endWrite(){}};
static struct {struct Display Display;Button BtnA,BtnB,BtnC;} M5;
static void drawMenuRow(Display&,int,const EntityState&,bool){}
static uint8_t g_wakeOnlyButtons=0;
static int nav=0;
static void goBack(){g_detail=false;++nav;}
static void openDetail(){g_detail=true;g_detailGlobal=globalSlot(g_focus);++nav;}
static void moveFocus(int delta){g_focus=(g_focus+delta+GRID_SLOTS)%GRID_SLOTS;++nav;}
static void acMove(int delta){g_acCursor=(g_acCursor+delta+AC_ROWS)%AC_ROWS;++nav;}
static void changePage(int){g_page=1-g_page;g_focus=0;++nav;}
static String nextCsv(const String& list,const String& current){
    auto pos=list.find(current);auto next=list.find(',',pos==String::npos?0:pos);
    return next==String::npos?list.substr(0,list.find(',')):list.substr(next+1,list.find(',',next+1)-(next+1));
}
static bool acNextMode(EntityState& e,int){return callService("climate","set_hvac_mode",e.id,"mode_from="+e.state);}
static void onHaState(const EntityState& state){
    for(int i=0;i<g_config.entityCount;++i){
        if(g_cards[i].id==state.id){auto aux=g_cards[i].aux;g_cards[i]=state;g_cards[i].aux=aux;}
        if(g_cards[i].aux.id==state.id){auto& a=g_cards[i].aux;a.available=state.available;a.state=state.state;a.on=state.on;
            a.type=state.type;a.revision=state.revision;a.brightness=state.brightness;a.brightnessAdjustable=state.brightnessAdjustable;}
    }
}
'''
tests = r'''
static EntityState state(const char* id,bool on=false){
    EntityState e;e.id=id;e.type=EntityType::Switch;e.available=true;e.on=on;e.state=on?"on":"off";return e;
}
static void reset(){
    g_config=AppConfig{};g_config.entityCount=16;
    for(auto& e:g_cards)e=EntityState{};
    g_cards[0]=state("switch.test");g_cards[1]=state("switch.other");
    g_page=g_focus=g_detailGlobal=g_acCursor=nav=0;g_detail=g_acEditing=false;g_acDraftTemp=NAN;
    g_backlightPhase=BacklightPhase::Normal;suspended=busy=false;now=generation=0;
    readComplete=commandComplete=nullptr;readDestination=nullptr;reads.clear();calls.clear();
    g_dimAction=DimInputAction{};g_dimButtonPress=false;g_dimButtonGeneration=0;g_wakeOnlyButtons=0;
    M5.BtnA=M5.BtnB=M5.BtnC=Button{};
}
static void dim(BacklightPhase phase=BacklightPhase::Dim){
    suspended=true;++generation;g_backlightPhase=phase;
    for(auto& e:g_cards){e.available=false;e.aux.available=false;}
}
static void pressB(bool held=false){
    M5.BtnB.pressed=M5.BtnB.pressEdge=true;M5.BtnB.longPress=held;
    noteButtonActivity();handleButtons();M5.BtnB.pressEdge=false;
}
static void releaseB(){
    M5.BtnB.pressed=M5.BtnB.longPress=false;M5.BtnB.releaseEdge=true;
    handleButtons();M5.BtnB.releaseEdge=false;
}
int main(){
    // Physical R5C1 dims directly, stays dim on another press, and cannot
    // dispatch a queued entity action. Serial . remains an independent key.
    reset();handleKey(remapKeypadKey('.'));assert(suspended && g_backlightPhase==BacklightPhase::Dim);
    auto dimGeneration=generation;handleKey(remapKeypadKey('.'));assert(generation==dimGeneration && suspended && calls.empty());
    reset();dim(BacklightPhase::Off);handleKey(remapKeypadKey('.'));
    assert(!suspended && g_backlightPhase==BacklightPhase::Normal && !dimActionPending());
    reset();dim();handleKey('e');dimActionTick();handleKey(remapKeypadKey('.'));
    finishRead(state("switch.test"));assert(calls.empty() && suspended);
    // A dim key triggers one targeted read before the command. Extra ticks and
    // repeated input cannot issue a second command while that read is pending.
    reset();dim();handleKey('e');assert(!suspended && calls.empty() && dimActionPending());
    dimActionTick();assert(reads==std::vector<String>{"switch.test"});
    for(int i=0;i<5;++i){handleKey('e');dimActionTick();}assert(calls.empty());
    finishRead(state("switch.test",true));assert(calls.size()==1 && calls[0].service=="toggle");
    finishCommand();assert(!g_cards[0].on); // Toggle uses the freshly read ON value.
    dimActionTick();assert(calls.size()==1 && !dimActionPending());
    // Selection/page changes while waiting cannot redirect the captured action.
    reset();dim();handleKey('e');dimActionTick();handleKey('d');handleKey('n');
    assert(nav==2 && g_page==1);finishRead(state("switch.test"));
    assert(calls.size()==1 && calls[0].id=="switch.test");
    // Numeric keys select and refresh their own slot, not the old focus.
    reset();dim();handleKey('2');assert(g_focus==1);dimActionTick();
    assert(reads.back()=="switch.other");finishRead(state("switch.other"));assert(calls[0].id=="switch.other");
    // Failed/unavailable reads never replay when later updates/retries arrive.
    for(bool transportFailure:{false,true}){
        reset();dim();handleKey('e');dimActionTick();auto fresh=state("switch.test");fresh.available=false;
        finishRead(fresh,!transportFailure);assert(calls.empty() && !dimActionPending());
        onHaState(state("switch.test"));dimActionTick();assert(calls.empty());
    }
    // A newer live update wins over an old REST response.
    reset();dim();handleKey('e');dimActionTick();auto live=state("switch.test",true);live.revision=9;onHaState(live);
    finishRead(state("switch.test",false));finishCommand();assert(!g_cards[0].on && g_cards[0].revision==9);
    // Re-suspension, deadline expiry, or a changed ID cancels the deferred action.
    for(int scenario:{0,1,2}){
        reset();dim();handleKey('e');dimActionTick();
        if(scenario==0){dim();noteActivity();}
        if(scenario==1)now=10000;
        if(scenario==2)g_cards[0].id="switch.changed";
        finishRead(state("switch.test"));assert(calls.empty());
    }
    // A new dim press can replace a cancelled generation, even while its old
    // read drains. That old callback must not consume the new action.
    reset();dim();handleKey('e');dimActionTick();dim();handleKey('e');
    finishRead(state("switch.test"));assert(calls.empty() && dimActionPending());
    dimActionTick();finishRead(state("switch.test"));assert(calls.size()==1);
    // Physical short B has identical semantics, using the target at press time.
    reset();dim();pressB();assert(calls.empty());g_focus=1;releaseB();dimActionTick();
    assert(reads.back()=="switch.test");finishRead(state("switch.test"));assert(calls[0].id=="switch.test");
    // Dim long B opens detail once and never toggles on release; off remains wake-only.
    reset();g_cards[0].type=EntityType::Climate;dim();pressB(true);assert(g_detail && nav==1);releaseB();dimActionTick();assert(calls.empty() && reads.empty());
    reset();dim(BacklightPhase::Off);pressB(true);releaseB();assert(!g_detail && calls.empty() && !dimActionPending());
    reset();dim(BacklightPhase::Off);handleKey('2');assert(g_focus==0 && !dimActionPending());
    reset();dim();handleKey('d');assert(g_focus==1 && !dimActionPending() && !busy);
    // Brightness and temperature adjustments calculate their target from fresh metadata.
    reset();g_cards[0].type=EntityType::Light;dim();handleKey('+');dimActionTick();
    auto fresh=state("switch.test",true);fresh.type=EntityType::Light;fresh.brightness=204;fresh.brightnessAdjustable=true;
    finishRead(fresh);assert(calls.size()==1 && calls[0].extra=="\"brightness_pct\":90");
    reset();g_cards[0].type=EntityType::Climate;dim();handleKey('-');dimActionTick();
    fresh=state("switch.test");fresh.type=EntityType::Climate;fresh.target=25;fresh.tempStep=.5;fresh.temperatureAdjustable=true;
    finishRead(fresh);assert(calls[0].extra=="\"temperature\":24.50");
    // AUX gets its own targeted refresh, and works even while the parent's value is stale.
    reset();g_cards[0].type=EntityType::Climate;g_detail=true;g_acCursor=5;
    g_cards[0].aux.id="switch.aux";g_cards[0].aux.type=EntityType::Switch;
    dim();handleKey('e');dimActionTick();assert(reads.back()=="switch.aux");
    finishRead(state("switch.aux"));assert(calls.size()==1 && calls[0].id=="switch.aux");
    // Changing the selected detail row cannot change the requested control.
    reset();g_cards[0].type=EntityType::Climate;g_detail=true;g_acCursor=2;
    dim();handleKey('e');dimActionTick();handleKey('d');assert(g_acCursor==3);
    fresh=state("switch.test");fresh.type=EntityType::Climate;fresh.fanMode="auto";fresh.fanModes="auto,low";
    finishRead(fresh);assert(calls.size()==1 && calls[0].service=="set_fan_mode" && calls[0].extra=="\"fan_mode\":\"low\"");
    // Editing is only entered if the original editor is still visible.
    reset();g_cards[0].type=EntityType::Climate;g_detail=true;g_acCursor=1;dim();handleKey('e');dimActionTick();
    fresh=state("switch.test");fresh.type=EntityType::Climate;fresh.target=23;fresh.temperatureAdjustable=true;
    finishRead(fresh);assert(g_acEditing && g_acDraftTemp==23 && calls.empty());
    std::puts("PASS: dim refresh-before-action once, target capture across navigation, numeric/Core inputs, failures, live races, cancellation/timeout, off wake-only, fresh adjustments and AUX");
}
'''
# Compile the real input/deferred-action code; only hardware and HA transport are mocked.
start = source.index('template<class Entity, class Update>')
code = prefix + source[start:source.index('static void acMove(', start)]
code += function('static bool validTemp(') + function('static void acAdjust(')
for signature in ['static void acCycleFan(', 'static void acCycleSwing(', 'static void acAuxAction(',
                  'static void acTempAction(', 'static void performAcAction(', 'static void acAction(',
                  'static void performPrimaryAction(']:
    code += function(signature)
code += 'static bool dimActionTargets(const String&);\n'
code += function('static void primaryAction(')
start = source.index('template<class Light>')
code += source[start:source.index('static void changePage(', start)]
start = source.index('enum class DimActionKind')
code += source[start:source.index('static void handleSerialInput(', start)]
code = code.replace('static uint8_t g_wakeOnlyButtons = 0;', '')  # Provided next to button mocks.
# Unused fake overload is unnecessary; all drawing uses the real signature.
code = code.replace('static void drawMenuRow(int,int,const EntityState&,bool){}\n', '')
with tempfile.TemporaryDirectory(prefix='m5-dim-actions-') as directory:
    tmp = Path(directory)
    (tmp / 'Arduino.h').write_text(arduino)
    (tmp / 'test.cpp').write_text(code + tests)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', f'-I{tmp}',
                    f'-I{ROOT / "include"}', str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)

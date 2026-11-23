#!/usr/bin/env python3
"""Exercise the firmware's actual input and adjustment functions with mocked HA."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/main.cpp').read_text()

def function(signature):
    start = source.index(signature)
    pos = source.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

arduino = r'''
#pragma once
#include <string>
#include <cmath>
#include <cstdint>
#include <cctype>
class String : public std::string {
public:
    using std::string::string;
    String() = default;
    String(const std::string& value):std::string(value){}
    String(int value):std::string(std::to_string(value)){}
    bool isEmpty() const { return empty(); }
    void toUpperCase(){for(char& c:*this)c=std::toupper(static_cast<unsigned char>(c));}
    void trim(){auto a=find_first_not_of(" \t\r\n"); if(a==npos){clear();return;}
        auto b=find_last_not_of(" \t\r\n"); *this=substr(a,b-a+1);}

};
'''
prefix = r'''
#include "ha_client.h"
#include "keypad_controls.h"
#include "utf8_text.h"
#include <ArduinoJson.h>
void convertFromJson(JsonVariantConst source, String& target){target=source.as<std::string>();}
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <vector>
using std::isfinite;
using std::isnan;
template<class T> static T constrain(T value,T low,T high){return std::min(high,std::max(low,value));}
struct Call { String domain,service,id,extra; };
static std::vector<Call> calls;
static bool serviceOk=true, g_detail=false, g_acEditing=false;
static bool suspended=false;
uint32_t haDataGeneration(){return 0;}
static bool dimActionTargets(const String&){return false;}
enum class DimActionKind {Primary,Adjust};
struct InputTarget{};
static InputTarget g_dimButtonTarget;
static uint32_t g_dimButtonGeneration=0;
static bool g_dimButtonPress=false;
static int queuedDimActions=0;
static InputTarget captureInputTarget(){return {};}
static void queueDimAction(const InputTarget&,DimActionKind,int=0){++queuedDimActions;}
bool haDataSuspended(){return suspended;}
static int g_acCursor=0,g_focus=0,refreshes=0,statusDraws=0,menuDraws=0,lastSlot=-1,actions=0;
static float g_acDraftTemp=NAN;
static EntityState entity;
static String menuValue;
static String entityStateText(const EntityState& e);
enum class BacklightPhase {Normal,Dim,Off};
static BacklightPhase g_backlightPhase=BacklightPhase::Normal;
bool setDisplayMode(const String& mode){assert(mode=="dim" || mode=="off");g_backlightPhase=mode=="dim"?BacklightPhase::Dim:BacklightPhase::Off;suspended=true;return true;}
static void noteActivity(){g_backlightPhase=BacklightPhase::Normal;}
static bool acDetailActive(){return g_detail && entity.type==EntityType::Climate;}
static EntityState& acEntity(){return entity;}
static EntityState& selectedEntity(){return entity;}
static EntityState& cardAt(int){return entity;}
String entityDomain(const String& id){return id.substr(0,id.find('.'));}
String defaultNameFromId(const String& id){return id;}
EntityType entityTypeFromId(const String& id){
    if(id.find("light.")==0)return EntityType::Light;
    if(id.find("climate.")==0)return EntityType::Climate;
    return EntityType::Switch;
}
static HaResultCallback pendingCommand;
static bool callService(const String& d,const String& s,const String& id,const String& x="",
                        HaResultCallback complete=nullptr){
    calls.push_back({d,s,id,x});
    if(serviceOk && complete) pendingCommand=complete;
    return serviceOk;
}
static void completeCommand(bool ok=true){
    auto complete=std::move(pendingCommand);pendingCommand=nullptr;
    if(complete)complete(ok);
    menuValue=entityStateText(entity);
}
static void redrawAcRow(int slot){lastSlot=slot;}
static void redrawRow(int,bool){++menuDraws;menuValue=entityStateText(entity);}
static void redrawStatusRegions(){++statusDraws;}
static void scheduleRefresh(){++refreshes;}
static void sfxArrow(){}
static void sfxSelect(){}
static void redrawAcDetail(){}
static void goBack(){g_detail=false;}
static void changePage(int){}
static void acMove(int){++actions;}
static void acTempAction();
static void acAction(){if(g_acEditing) acTempAction(); else ++actions;}
static void moveFocus(int){++actions;}
static void primaryAction(){if(g_detail)acAction();else ++actions;}
static void openDetail(){g_detail=true;}
static void directSelect(int slot,bool=false){lastSlot=slot;}
struct Button {
    bool pressed=false,pressEdge=false,releaseEdge=false,longPress=false;
    bool isPressed(){return pressed;}
    bool wasPressed(){return pressEdge;}
    bool wasReleased(){return releaseEdge;}
    bool pressedFor(int){return longPress;}
};
static struct {Button BtnA,BtnB,BtnC;} M5;
static uint8_t g_wakeOnlyButtons=0;
static constexpr int AC_ROWS=7;
'''
tests = r'''
static void reset(){
    suspended=false;pendingCommand=nullptr;calls.clear();entity=EntityState{};serviceOk=true;g_detail=g_acEditing=false;
    menuValue.clear();
    g_acCursor=g_focus=refreshes=statusDraws=menuDraws=actions=0;lastSlot=-1;g_acDraftTemp=NAN;
    g_backlightPhase=BacklightPhase::Normal;g_wakeOnlyButtons=0;
    g_dimButtonPress=false;queuedDimActions=0;
    M5.BtnA=M5.BtnB=M5.BtnC=Button{};
}
static void light(){
    reset();entity.id="light.lamp";entity.type=EntityType::Light;entity.available=true;
    entity.brightnessAdjustable=true;entity.on=true;entity.state="on";entity.brightness=128;
}
static void climate(){
    reset();entity.id="climate.ac";entity.type=EntityType::Climate;entity.available=true;
    entity.temperatureAdjustable=true;entity.target=24;entity.tempStep=1;entity.minTemp=18;entity.maxTemp=26;
    entity.state="cool";entity.current=27;
}
static EntityState parse(const char* id,const char* json){
    JsonDocument document;assert(!deserializeJson(document,json));EntityState result;result.id=id;
    assert(parseState(result,document.as<JsonVariantConst>()));return result;
}
int main(){
    reset();entity.id="sensor.temperature";entity.type=EntityType::Sensor;
    entity.available=true;entity.state="23.4";
    for(const char* unit: {"°C","℃"}){
        entity.unit=unit;assert(entityStateText(entity)=="23.4 C");
    }
    for(const char* unit: {"°F","℉"}){
        entity.unit=unit;assert(entityStateText(entity)=="23.4 F");
    }
    entity.unit="%";assert(entityStateText(entity)=="23.4 %");
    entity.aux.id="sensor.aux_temperature";entity.aux.available=true;
    entity.aux.type=EntityType::Sensor;entity.aux.state="22.0";entity.aux.unit="°C";
    assert(acAuxText(entity)=="22.0 C");
    assert(parse("sensor.temperature",R"({"state":"23.4","attributes":{"unit_of_measurement":"℃"}})").unit=="C");
    assert(parse("sensor.temperature",R"({"state":"23.4","attributes":{"unit_of_measurement":"℉"}})").unit=="F");
    assert(parse("light.test",R"({"state":"off","attributes":{"supported_color_modes":["brightness"],"brightness":null}})").brightnessAdjustable);
    assert(!parse("light.test",R"({"state":"on","attributes":{"supported_color_modes":["onoff"],"brightness":128}})").brightnessAdjustable);
    assert(parse("light.test",R"({"state":"on","attributes":{"supported_color_modes":["hs"]}})").brightnessAdjustable);
    assert(parse("light.test",R"({"state":"on","attributes":{"supported_features":1}})").brightnessAdjustable);
    assert(parse("light.test",R"({"state":"on","attributes":{"brightness":128}})").brightnessAdjustable);
    assert(!parse("switch.test",R"({"state":"on","attributes":{"supported_features":1}})").brightnessAdjustable);
    assert(parse("climate.test",R"({"state":"off","attributes":{"supported_features":1,"temperature":null}})").temperatureAdjustable);
    assert(!parse("climate.test",R"({"state":"auto","attributes":{"supported_features":2}})").temperatureAdjustable);
    assert(!parse("climate.test",R"({"state":"on","attributes":{"supported_features":0,"temperature":24}})").temperatureAdjustable);
    auto metadata=parse("climate.test",R"({"state":"cool","attributes":{"temperature":24,"target_temp_step":1,"min_temp":18,"max_temp":26}})");
    assert(metadata.temperatureAdjustable && metadata.tempStep==1 && metadata.minTemp==18 && metadata.maxTemp==26);
    const char raw[]={'7','8','9','4','5','6','1','2','3','-','+'};
    const char logical[]={'1','2','3','4','5','6','7','8','9','+','-'};
    for(unsigned i=0;i<sizeof(raw);++i) assert(remapKeypadKey(raw[i])==logical[i]);
    for(char c: {'A','\b','M','%','=','\r','0'}) assert(remapKeypadKey(c)==c);
    assert(remapKeypadKey('.')==KEYPAD_DIM_KEY);
    const uint8_t ignored[]={0,255,'/','*','`'};
    for(uint8_t c: ignored) assert(remapKeypadKey(c)==0);
    light();handleKey(remapKeypadKey('.'));
    assert(g_backlightPhase==BacklightPhase::Dim && suspended && calls.empty() && actions==0);
    handleKey(remapKeypadKey('.'));assert(g_backlightPhase==BacklightPhase::Off && calls.empty());
    handleKey(remapKeypadKey('.'));assert(g_backlightPhase==BacklightPhase::Normal && calls.empty());
    climate();handleKey('.');assert(g_detail && g_backlightPhase==BacklightPhase::Normal); // Serial . retains detail.
    light();g_backlightPhase=BacklightPhase::Off;handleKey(remapKeypadKey('.'));
    assert(g_backlightPhase==BacklightPhase::Normal && !g_detail && calls.empty());
    reset();handleKey(remapKeypadKey('7'));assert(lastSlot==0);
    handleKey('7');assert(lastSlot==6); // Serial numbers are not remapped.

    light();handleKey(remapKeypadKey('-'));completeCommand();
    assert(calls.size()==1 && calls[0].id=="light.lamp" && calls[0].service=="turn_on");
    assert(calls[0].extra=="\"brightness_pct\":60" && entity.brightness==153);
    assert(refreshes==1 && menuDraws==1);
    assert(menuValue=="ON 60%");
    light();handleKey(remapKeypadKey('+'));completeCommand();assert(calls[0].extra=="\"brightness_pct\":40");
    light();entity.brightness=255;handleKey('+');completeCommand();assert(calls.empty());
    light();entity.on=false;entity.brightness=128;handleKey('-');completeCommand();assert(calls.empty());
    handleKey('+');completeCommand();assert(calls[0].extra=="\"brightness_pct\":10");
    light();entity.brightness=26;handleKey('-');completeCommand();
    assert(calls[0].service=="turn_off" && calls[0].extra.empty() && !entity.on);
    light();entity.brightnessAdjustable=false;handleKey('+');completeCommand();assert(calls.empty());
    light();entity.available=false;handleKey('+');completeCommand();assert(calls.empty());
    light();entity.type=EntityType::Sensor;handleKey('+');completeCommand();assert(calls.empty());
    light();serviceOk=false;handleKey('+');completeCommand();assert(entity.brightness==128 && refreshes==0 && menuDraws==0);

    climate();handleKey('+');completeCommand();assert(entity.target==25 && calls[0].extra=="\"temperature\":25.00");
    assert(entity.current==27 && menuValue=="COOL 25.0C");
    assert(!entity.on); // Adjusting a value does not optimistically turn the AC on.
    climate();handleKey('-');completeCommand();assert(menuValue=="COOL 23.0C" && entity.current==27);
    climate();entity.target=NAN;assert(entityStateText(entity)=="COOL 27.0C");
    entity.current=NAN;assert(entityStateText(entity)=="COOL");
    entity.available=false;assert(entityStateText(entity)=="--");
    climate();entity.tempStep=0.25;handleKey('-');completeCommand();assert(entity.target==23.75f);
    climate();entity.target=26;handleKey('+');completeCommand();assert(calls.empty());
    climate();entity.target=18;handleKey('-');completeCommand();assert(calls.empty());
    climate();entity.target=25.5;handleKey('+');completeCommand();assert(entity.target==26);
    climate();entity.temperatureAdjustable=false;handleKey('+');completeCommand();assert(calls.empty());
    climate();entity.available=false;handleKey('+');completeCommand();assert(calls.empty());
    climate();serviceOk=false;handleKey('+');completeCommand();assert(entity.target==24 && refreshes==0);
    climate();entity.minTemp=30;entity.maxTemp=18;handleKey('+');completeCommand();assert(calls.empty());

    climate();g_detail=true;g_acCursor=1;handleKey('+');completeCommand();assert(entity.target==25 && lastSlot==1);
    climate();g_detail=true;g_acCursor=0;handleKey('+');completeCommand();assert(calls.empty());
    climate();g_detail=true;g_acCursor=1;g_acEditing=true;g_acDraftTemp=24;
    handleKey('+');completeCommand();assert(calls.empty() && g_acDraftTemp==25 && entity.target==24 && g_acEditing);
    handleKey('=');completeCommand();assert(calls.size()==1 && entity.target==25 && !g_acEditing && !entity.on);
    climate();g_detail=true;g_acCursor=1;g_acEditing=true;g_acDraftTemp=24.25;serviceOk=false;
    handleKey('=');completeCommand();assert(g_acEditing && entity.target==24);
    climate();g_detail=true;g_acCursor=5;entity.aux.id="light.aux";entity.aux.type=EntityType::Light;
    entity.aux.available=entity.aux.on=entity.aux.brightnessAdjustable=true;entity.aux.brightness=128;
    handleKey('+');completeCommand();assert(calls.size()==1 && calls[0].id=="light.aux" && lastSlot==5);

    light();g_backlightPhase=BacklightPhase::Off;handleKey('+');completeCommand();
    assert(calls.empty() && g_backlightPhase==BacklightPhase::Normal);
    handleKey('+');completeCommand();assert(calls.size()==1);
    light();g_backlightPhase=BacklightPhase::Dim;handleKey('+');completeCommand();assert(calls.empty() && queuedDimActions==1);
    handleKey('+');completeCommand();assert(calls.size()==1);
    for(auto phase:{BacklightPhase::Dim,BacklightPhase::Off}){
        reset();g_backlightPhase=phase;
        M5.BtnB.pressed=M5.BtnB.pressEdge=M5.BtnB.longPress=true;
        noteButtonActivity();handleButtons();
        assert(g_backlightPhase==BacklightPhase::Normal && g_detail==(phase==BacklightPhase::Dim) && actions==0);
        M5.BtnB.pressEdge=false;noteButtonActivity();handleButtons();
        assert(g_detail==(phase==BacklightPhase::Dim) && actions==0); // Dim holds navigate; off holds stay wake-only.
        M5.BtnB.pressed=M5.BtnB.longPress=false;M5.BtnB.releaseEdge=true;
        handleButtons();assert(actions==0 && g_wakeOnlyButtons==0);
        M5.BtnB.releaseEdge=false;M5.BtnB.pressed=M5.BtnB.pressEdge=true;
        noteButtonActivity();handleButtons();
        M5.BtnB.pressed=M5.BtnB.pressEdge=false;M5.BtnB.releaseEdge=true;
        handleButtons();assert(actions==1); // A separate press acts normally.
    }
    light();suspended=true;assert(entityStateText(entity)=="--");
    entity.aux.id="sensor.aux";entity.aux.available=true;entity.aux.state="23";
    assert(acAuxText(entity)=="--");
    climate();suspended=true;g_acDraftTemp=25;
    assert(acTempText(entity,true)=="--" && acCurrentText(entity)=="--");
    suspended=false;entity.available=false;
    assert(acTempText(entity,true)=="--" && acCurrentText(entity)=="--");
    // Reproduce the original double inversion with the actual toggle helper.
    light();assert(toggleEntity(entity,"light"));
    assert(entity.on); // Merely queueing cannot change the cached state.
    entity.on=false;entity.state="off";++entity.revision;
    completeCommand();assert(!entity.on && entity.state=="off");
    light();assert(toggleEntity(entity,"light"));completeCommand();
    assert(!entity.on && entity.state=="off");
    light();assert(toggleEntity(entity,"light"));completeCommand(false);
    assert(entity.on && entity.state=="on");
    light();entity.aux.id="switch.aux";entity.aux.on=true;entity.aux.state="on";
    assert(toggleEntity(entity.aux,"homeassistant"));
    entity.aux.on=false;entity.aux.state="off";++entity.aux.revision;
    completeCommand();assert(!entity.aux.on && entity.aux.state=="off");
    // Navigating to another entity cannot redirect a pending command result.
    light();EntityState original=entity;
    assert(toggleEntity(original,"light"));
    entity.id="light.other";entity.on=true;entity.state="on";
    completeCommand();assert(!original.on && entity.on && entity.id=="light.other");
    // HTTP completion must never overwrite a newer live value (primary/AUX).
    light();assert(adjustLightBrightness(entity,1));
    assert(entity.brightness==128); // Submission is not completion.
    entity.brightness=220;entity.on=false;entity.state="off";++entity.revision;
    completeCommand();assert(entity.brightness==220 && !entity.on && entity.state=="off");
    light();assert(adjustLightBrightness(entity,1));completeCommand(false);
    assert(entity.brightness==128 && entity.on);
    climate();assert(adjustClimateTemperature(entity,1));
    entity.target=20;++entity.revision;completeCommand();assert(entity.target==20);
    light();entity.aux.id="light.aux";entity.aux.available=true;
    entity.aux.on=entity.aux.brightnessAdjustable=true;entity.aux.brightness=128;
    assert(adjustLightBrightness(entity.aux,1));entity.aux.brightness=40;++entity.aux.revision;
    completeCommand();assert(entity.aux.brightness==40);
    std::puts("PASS: physical remap, serial keys, light/AC adjustments and limits, unsupported entities, failed commands, AC detail/drafts/AUX, dim action routing, off wake-only, held buttons, stale values and command races");
}
'''
client = (ROOT / 'src/ha_client.cpp').read_text()
original_source=source
source=client
client_functions=''.join(function(signature) for signature in ['String displayText(', 'static void deriveOn(', 'static String joinArray(', 'static bool parseState('])
source=original_source
body = prefix + client_functions + function('static bool validTemp(')
body += function('static String upperText(') + function('static String tempText(')
body += function('static String displayUnit(') + function('static String acAuxText(')
body += function('static String acTempText(') + function('static String acCurrentText(')
body += function('static int lightPercent(') + function('static String entityStateText(')
start=source.index('template<class Entity, class Update>')
body += source[start:source.index('static void acMove(',start)]
body += function('static void acAdjust(') + function('static void acTempAction(')
start = source.index('template<class Light> static bool adjustLightBrightness(')
body += source[start:source.index('static void changePage(',start)]
body += function('static void noteButtonActivity(') + function('static void handleButtons(')
body += function('static void handleKey(') + tests
with tempfile.TemporaryDirectory(prefix='m5-controls-') as directory:
    tmp=Path(directory)
    (tmp/'Arduino.h').write_text(arduino)
    (tmp/'test.cpp').write_text(body)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{tmp}',f'-I{ROOT / "include"}',f'-I{ROOT / ".pio/libdeps/m5stack-core/ArduinoJson/src"}',str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)

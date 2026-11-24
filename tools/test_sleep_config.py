#!/usr/bin/env python3
"""Exercise actual web/NVS settings, shared sleep timers and old-config migration."""
import ast
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'src/app_config.cpp').read_text()
module = ast.parse((ROOT/'tools/test_controls.py').read_text())
arduino = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'arduino' for t in n.targets))
arduino = arduino.replace('#include <string>', '#include <string>\n#include <cstdlib>')
arduino = arduino.replace('    void toUpperCase()', '''
    bool startsWith(const char* text)const{return rfind(text,0)==0;}
    bool endsWith(const char* text)const{auto n=std::char_traits<char>::length(text);return size()>=n && compare(size()-n,n,text)==0;}
    void remove(size_t from){erase(from);}
    String substring(size_t from,size_t to=npos)const{return substr(from,to==npos?npos:to-from);}
    int indexOf(char c)const{auto p=find(c);return p==npos?-1:int(p);}
    int toInt()const{return std::strtol(c_str(),nullptr,10);}
    void toLowerCase(){for(char& c:*this)c=std::tolower(static_cast<unsigned char>(c));}
    void toUpperCase()''')

def function(signature):
    start = source.index(signature)
    end = source.index('{', start)+1
    depth = 1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end += 1
    return source[start:end]+'\n'

prefix = r'''
#include "app_config.h"
#include "connected_power.h"
#include <algorithm>
#include <cassert>
#include <map>
#include <cstdio>
AppConfig g_config;
void beginConnectedPowerWork(){}
void endConnectedPowerWork(){}
template<class T>static T constrain(T value,T low,T high){return std::min(high,std::max(low,value));}
static std::map<String,int> ints;
static std::map<String,String> strings;
struct Preferences{
    bool begin(const char*,bool){return true;}
    void end(){}
    bool isKey(const char* key){return ints.count(key)||strings.count(key);}
    int getInt(const char* key,int fallback){return ints.count(key)?ints[key]:fallback;}
    String getString(const char* key,const char* fallback){return strings.count(key)?strings[key]:String(fallback);}
    size_t putInt(const char* key,int value){ints[key]=value;return sizeof(value);}
    size_t putString(const char* key,const String& value){strings[key]=value;return value.size();}
    bool remove(const char* key){return ints.erase(key)+strings.erase(key)>0;}
};
struct Server{
    std::map<String,String> args;
    int status=0;
    String arg(const String& key){return args.count(key)?args[key]:String("");}
    bool hasArg(const String& key){return args.count(key);}
    void send(int code,const char*,const String&){status=code;}
}server;
static int saveErrors=0;
static void sendSaveError(const String&){++saveErrors;server.status=400;}
static String htmlHeader(const String&){return "";}
static String htmlFooter(){return "";}
static void delay(int){}
struct{template<class... T>void printf(const char*,T...){}void println(const char*){}void flush(){}}Serial;
struct{int reboots=0;void restart(){++reboots;}}ESP;
'''
tests = r'''
static void form(const char* light="1",const char* deep="30"){
    server.args={{"ssid","test"},{"host","ha-remote"},{"url","http://ha"},{"bright","30"},
                 {"dstart","07:00"},{"dend","21:00"},{"dbright","5"},{"ddimbright","1"},
                 {"ddim","300"},{"doff","0"},{"nstart","21:00"},{"nend","07:00"},
                 {"nbright","5"},{"ndimbright","1"},{"ndim","60"},{"noff","3600"},
                 {"pagecount","1"},{"e0","switch.test"},{"ajax","1"},{"lightsleep",light},{"deepsleep",deep}};
}
static void reload(){g_config=AppConfig{};configLoad();}
static void timers(){assert(g_config.dayDimSeconds==300 && g_config.dayOffSeconds==0 && g_config.nightDimSeconds==60 && g_config.nightOffSeconds==3600);}
int main(){
    ints={{"dd",300},{"do",0},{"nd",60},{"no",3600}};
    strings={{"ssid","test"},{"token","saved-secret"}};
    ints["bright"]=30;ints["dsm"]=1;ints["nsm"]=0;
    reload();timers();assert(g_config.lightSleepMinutes==1 && g_config.deepSleepMinutes==30);
    assert(g_config.lowBatteryWarning); // Existing configurations enable the new warning by default.
    form();handleSave();assert(server.status==200 && ESP.reboots==1);
    assert(ints["lsm"]==1 && ints["dpm"]==30);
    assert(!ints.count("bright") && !ints.count("dsm") && !ints.count("nsm"));
    reload();timers();assert(g_config.lightSleepMinutes==1 && g_config.deepSleepMinutes==30);
    assert(g_config.haToken=="saved-secret"); // Blank password/token fields preserve credentials.
    // Missing timer fields preserve the shared plan; each timer can be disabled.
    form("2","90");handleSave();reload();timers();
    assert(g_config.lightSleepMinutes==2 && g_config.deepSleepMinutes==90);
    form();server.args.erase("lightsleep");server.args.erase("deepsleep");handleSave();reload();timers();
    assert(g_config.lightSleepMinutes==2 && g_config.deepSleepMinutes==90);
    form("0","0");handleSave();reload();
    assert(g_config.lightSleepMinutes==0 && g_config.deepSleepMinutes==0);
    for(const char* key:{"lightsleep","deepsleep"}) for(const char* bad:{"", "-1", "1.5", "1441", "oops", "999999"}){
        form();server.args[key]=bad;server.args["ssid"]="must-not-apply";
        int boots=ESP.reboots,errors=saveErrors;auto saved=ints;String ssid=g_config.ssid;
        handleSave();assert(server.status==400 && ESP.reboots==boots && saveErrors==errors+1);
        assert(ints==saved && g_config.ssid==ssid);
    }
    form("1440","1440");handleSave();reload();
    assert(g_config.lightSleepMinutes==1440 && g_config.deepSleepMinutes==1440);
    form();server.args["lowbat"]="off";handleSave();reload();
    assert(!g_config.lowBatteryWarning && ints["lowbat"]==0);
    form();handleSave();reload();assert(!g_config.lowBatteryWarning); // Older forms preserve the choice.
    form();server.args["lowbat"]="invalid";handleSave();reload();assert(!g_config.lowBatteryWarning);
    form();server.args["lowbat"]="on";handleSave();reload();
    assert(g_config.lowBatteryWarning && ints["lowbat"]==1);
    // Legacy ends no longer create gaps or overlaps: only the two starts survive.
    ints["ds"]=420;ints["ns"]=1260;ints["de"]=1200;ints["ne"]=480;
    reload();assert(g_config.dayStart==420 && g_config.nightStart==1260);
    assert(configSave() && !ints.count("de") && !ints.count("ne"));
    ints["ds"]=100;ints["ns"]=100;reload();
    assert(g_config.dayStart==100 && g_config.nightStart==820);
    assert(configSave());
    // Browser bypass cannot persist an invalid/equal schedule or partially change config.
    for (const char* invalid : {"07:00", "", "24:00", "12:60", "xx:30", "7:00", "07:00x"}) {
        form();server.args["nstart"]=invalid;server.args["ssid"]="must-not-apply";
        const auto savedInts=ints;const auto savedStrings=strings;
        const String ssid=g_config.ssid;const int boots=ESP.reboots,errors=saveErrors;
        handleSave();assert(server.status==400 && saveErrors==errors+1 && ESP.reboots==boots);
        assert(ints==savedInts && strings==savedStrings && g_config.ssid==ssid);
    }
    form();server.args["dstart"]="23:00";server.args["nstart"]="06:00";
    handleSave();reload();assert(g_config.dayStart==1380 && g_config.nightStart==360);
    form();server.args.erase("dstart");server.args.erase("nstart");handleSave();reload();
    assert(g_config.dayStart==1380 && g_config.nightStart==360);
    g_config.nightStart=g_config.dayStart;assert(!configSave());reload();
    assert(g_config.dayStart!=g_config.nightStart);
    std::puts("PASS: linked start times, legacy gap/equal-time migration, validation before mutation, overnight schedules and missing-field preservation");
    std::puts("PASS: shared minute timers, disabled stages, old-config cleanup, malformed input, NVS roundtrip and preserved display settings/credentials");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-sleep-config-') as directory:
    tmp = Path(directory)
    (tmp/'Arduino.h').write_text(arduino)
    constants = source[source.index('static const char* NVS_NAMESPACE'):source.index('\n\nAppConfig g_config;')]
    code = prefix+constants+'\n'+''.join(function(s) for s in (
        'static String sanitizeHostname(', 'static void normalizeAuxEntities(',
        'void configLoad(', 'bool configSave(', 'static int parseTime(', 'static int parseSleepMinutes(', 'static void handleSave('))
    (tmp/'test.cpp').write_text(code+tests)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{tmp}',f'-I{ROOT/"include"}',
                    str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)

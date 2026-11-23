#!/usr/bin/env python3
"""Exercise actual web save and NVS load/save for independent day/night sleep modes."""
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
static void sendSaveError(const String&){assert(false);}
static String htmlHeader(const String&){return "";}
static String htmlFooter(){return "";}
static void delay(int){}
struct{template<class... T>void printf(const char*,T...){}void println(const char*){}void flush(){}}Serial;
struct{int reboots=0;void restart(){++reboots;}}ESP;
'''
tests = r'''
static void form(const char* day="deep",const char* night="light"){
    server.args={{"ssid","test"},{"host","ha-remote"},{"url","http://ha"},{"bright","30"},
                 {"dstart","07:00"},{"dend","21:00"},{"dbright","5"},{"ddimbright","1"},
                 {"ddim","300"},{"doff","0"},{"nstart","21:00"},{"nend","07:00"},
                 {"nbright","5"},{"ndimbright","1"},{"ndim","60"},{"noff","3600"},
                 {"pagecount","1"},{"e0","switch.test"},{"ajax","1"},{"dsleep",day},{"nsleep",night}};
}
static void reload(){g_config=AppConfig{};configLoad();}
static void timers(){assert(g_config.dayDimSeconds==300 && g_config.dayOffSeconds==0 && g_config.nightDimSeconds==60 && g_config.nightOffSeconds==3600);}
int main(){
    ints={{"dd",300},{"do",0},{"nd",60},{"no",3600}};
    strings={{"ssid","test"},{"token","saved-secret"}};
    reload();timers();assert(g_config.daySleepMode==SleepMode::Light && g_config.nightSleepMode==SleepMode::Light);
    assert(g_config.lowBatteryWarning); // Existing configurations enable the new warning by default.
    form();handleSave();assert(server.status==200 && ESP.reboots==1);
    assert(ints["dsm"]==1 && ints["nsm"]==0);
    reload();timers();assert(g_config.daySleepMode==SleepMode::Deep && g_config.nightSleepMode==SleepMode::Light);
    assert(g_config.haToken=="saved-secret"); // Blank password/token fields preserve credentials.
    // Older forms and invalid mode values must preserve the existing selections.
    form();server.args.erase("dsleep");server.args.erase("nsleep");handleSave();reload();timers();
    assert(g_config.daySleepMode==SleepMode::Deep && g_config.nightSleepMode==SleepMode::Light);
    form("invalid","2");handleSave();reload();
    assert(g_config.daySleepMode==SleepMode::Deep && g_config.nightSleepMode==SleepMode::Light);
    form("light","deep");handleSave();reload();timers();
    assert(g_config.daySleepMode==SleepMode::Light && g_config.nightSleepMode==SleepMode::Deep);
    assert(ints["dsm"]==0 && ints["nsm"]==1);
    // Corrupt/unrecognized NVS values default to the established Light behavior.
    ints["dsm"]=-1;ints["nsm"]=2;reload();timers();
    assert(g_config.daySleepMode==SleepMode::Light && g_config.nightSleepMode==SleepMode::Light);
    form();server.args["lowbat"]="off";handleSave();reload();
    assert(!g_config.lowBatteryWarning && ints["lowbat"]==0);
    form();handleSave();reload();assert(!g_config.lowBatteryWarning); // Older forms preserve the choice.
    form();server.args["lowbat"]="invalid";handleSave();reload();assert(!g_config.lowBatteryWarning);
    form();server.args["lowbat"]="on";handleSave();reload();
    assert(g_config.lowBatteryWarning && ints["lowbat"]==1);
    std::puts("PASS: actual web save/NVS roundtrip, independent sleep modes, old forms, invalid values, preserved timers and credentials");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-sleep-config-') as directory:
    tmp = Path(directory)
    (tmp/'Arduino.h').write_text(arduino)
    constants = source[source.index('static const char* NVS_NAMESPACE'):source.index('\n\nAppConfig g_config;')]
    code = prefix+constants+'\n'+''.join(function(s) for s in (
        'static String sanitizeHostname(', 'static void normalizeAuxEntities(', 'static SleepMode parseSleepMode(',
        'void configLoad(', 'bool configSave(', 'static int parseTime(', 'static void handleSave('))
    (tmp/'test.cpp').write_text(code+tests)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{tmp}',f'-I{ROOT/"include"}',
                    str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)

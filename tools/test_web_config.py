#!/usr/bin/env python3
"""Render the real setup handler with limited String allocations and a mock HTTP sink."""
import ast
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/app_config.cpp').read_text()


def function(signature):
    start = source.index(signature)
    pos = source.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


module = ast.parse((ROOT / 'tools/test_controls.py').read_text())
arduino = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'arduino' for t in n.targets))
arduino = arduino.replace('#include <string>', '#include <string>\n#include <cassert>')
arduino = arduino.replace('    String() = default;', '''
    String() = default;
    String(const char* value):std::string(value){assert(size()<=8192);}
    void reserve(size_t count){assert(count<=8192);std::string::reserve(count);}
    bool startsWith(const char* prefix)const{return rfind(prefix,0)==0;}
''')
arduino = arduino.replace('std::string(value){}', 'std::string(value){assert(size()<=8192);}')
prefix = r'''
#include "app_config.h"
#include "connected_power.h"
void beginConnectedPowerWork(){}
void endConnectedPowerWork(){}
#include "ha_client.h"
#include "html_stream.h"
#include "power_status.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
template<class T>static T constrain(T value,T low,T high){return std::min(high,std::max(low,value));}
static constexpr size_t CONTENT_LENGTH_UNKNOWN=static_cast<size_t>(-1);
struct WebServer {
    std::string body;
    std::vector<size_t> chunks;
    bool headers=false,terminated=false;
    void sendHeader(const char*,const char*){}
    void setContentLength(size_t length){assert(length==CONTENT_LENGTH_UNKNOWN);}
    void send(int status,const char*,const char* content){assert(status==200 && !*content);headers=true;}
    void sendContent(const char* content,size_t length){
        assert(headers && !terminated && length<=1024);
        if(!length){terminated=true;return;}
        body.append(content,length);chunks.push_back(length);
    }
};
static WebServer server;
AppConfig g_config;
static bool g_apMode=false;
static constexpr int WL_CONNECTED=3;
struct {int status(){return WL_CONNECTED;}String SSID(){return "test";}} WiFi;
static uint32_t millis(){return 1000;}
static int serviced=0;
void haLoop(){++serviced;}
bool haWsConnected(){return true;}
bool haDataSuspended(){return false;}
String networkIp(){return "192.0.2.1";}
String haTimeZone(){return "Asia/Shanghai";}
String haLocalTimeText(){return "2026-11-21T12:00:00";}
static PowerStatus testPower;
static bool testDock=false;
bool dockConnected(){return testDock;}
PowerStatus readPowerStatus(bool force){assert(force);return testPower;}
static HaOptionsCallback optionsComplete;
bool haFetchEntityOptions(HaOptionsCallback complete,int){optionsComplete=complete;return true;}
'''
tests = r'''
int main(int argc,char** argv){
    // An unavailable reading must not look like a genuinely empty battery.
    testPower.batteryPercent=-1;
    assert(powerIcon().find("Battery status unavailable")!=String::npos);
    assert(powerIcon().find(">?</text>")!=String::npos);
    testPower.batteryPercent=0;
    assert(powerIcon().find("Battery: 0%")!=String::npos);
    assert(powerIcon().find(">?</text>")==String::npos);
    testPower.batteryPercent=100;
    assert(powerIcon().find("Battery: 100%")!=String::npos);
    assert(powerIcon().find("width=\"32\"")!=String::npos);
    assert(dockIcon().find("aria-label=\"Base powered; charging available\"")!=String::npos);
    testPower.batteryPercent=75;
    g_config.ssid="test";g_config.hostname="test-remote";g_config.haUrl="http://ha";g_config.haToken="test";
    for(int p=0;p<MAX_PAGES;++p)g_config.pageTitles[p]="Room & lights";
    for(int i=0;i<MAX_ENTITIES;++i){
        g_config.entities[i]="climate.ac";g_config.entityNames[i]="AC <room>";
        g_config.auxEntities[i]="sensor.temperature";g_config.auxNames[i]="Temperature";
        g_config.climateModes[i]=CLIMATE_COOL|CLIMATE_HEAT;
    }
    for(int pages: {1,3,MAX_PAGES}){
        testDock=pages==3;
        server=WebServer{};serviced=0;g_config.pageCount=pages;g_config.entityCount=pages*GRID_SLOTS;
        g_config.lightSleepMinutes=pages==3?0:1;
        g_config.deepSleepMinutes=pages==3?0:30;
        g_config.lowBatteryWarning=pages!=3;
        handleRoot();
        if(optionsComplete){
            auto complete=std::move(optionsComplete);optionsComplete=nullptr;
            complete(true,"<option value=\"climate.ac\">AC</option>");
        }
        const auto& html=server.body;
        assert(server.headers && server.terminated && serviced==static_cast<int>(server.chunks.size()));
        assert(html.find("<script>")!=std::string::npos && html.find("</script>")!=std::string::npos);
        assert(html.find("const MAX_PAGES = 12;")!=std::string::npos);
        assert(html.find("const SLOTS = 8;")!=std::string::npos);
        assert(html.find("<label>Day starts<input id=\"dstart\" name=\"dstart\" type=\"time\" required")!=std::string::npos);
        assert(html.find("<label>Night starts<input id=\"nstart\" name=\"nstart\" type=\"time\" required")!=std::string::npos);
        assert(html.find("name=\"dend\"")==std::string::npos && html.find("name=\"nend\"")==std::string::npos);
        assert(html.find("function validateSchedule()")!=std::string::npos && html.find("form.reportValidity()")!=std::string::npos);
        assert(html.find("function movePage(")!=std::string::npos);
        assert(html.find("function openConnection(")!=std::string::npos);
        assert(html.find(testDock?"id=\"dock-status\" role=\"status\">":"id=\"dock-status\" role=\"status\" hidden>")!=std::string::npos);
        assert(html.find("fetch('/dock'")!=std::string::npos);
        assert(html.find("new FormData(form)")!=std::string::npos);
        auto power=html.substr(html.find("<legend>Power</legend>"));
        power=power.substr(0,power.find("</fieldset>"));
        auto lowbat=power.find("name=\"lowbat\">");assert(lowbat!=std::string::npos);
        auto options=power.substr(lowbat,power.find("</select>",lowbat)-lowbat);
        assert(options.find(g_config.lowBatteryWarning?"value=\"on\" selected":"value=\"off\" selected")!=std::string::npos);
        assert(html.find("Fallback brightness")==std::string::npos && html.find("name=\"bright\"")==std::string::npos);
        assert(html.find("name=\"dsleep\"")==std::string::npos && html.find("name=\"nsleep\"")==std::string::npos);
        auto sleep=html.find("<fieldset><legend>Sleep(On battery)</legend>");assert(sleep!=std::string::npos);
        auto section=html.substr(sleep,html.find("</fieldset>",sleep)-sleep);
        assert(section.find("Off &rarr; Light sleep (min)")!=std::string::npos);
        assert(section.find("Light sleep &rarr; Deep sleep (min)")!=std::string::npos);
        for(const char* field:{"lightsleep","deepsleep"}){
            auto name=std::string("name=\"")+field+"\"";
            auto pos=section.find(name);assert(pos!=std::string::npos && section.find(name,pos+1)==std::string::npos);
        }
        assert(html.find("</body></html>")!=std::string::npos);
        assert(html.find("Room &amp; lights")!=std::string::npos && html.find("AC &lt;room&gt;")!=std::string::npos);
        const std::string lastField="name=\"e"+std::to_string(pages*GRID_SLOTS-1)+"\"";
        assert(html.find(lastField)!=std::string::npos);
        if(pages==MAX_PAGES)assert(html.size()>100000);
        if(pages==MAX_PAGES && argc==2)std::ofstream(argv[1])<<html;
    }
    // A literal much larger than any permitted String still streams intact.
    server=WebServer{};server.headers=true;std::string literal(64000,'x');
    HtmlStream<WebServer> stream(server);stream+=literal.c_str();stream.finish();
    assert(server.body==literal && server.terminated);
    std::puts("PASS: complete config HTML and script for 1/3/12 pages, bounded chunks, limited String allocations, escaping and network servicing");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-web-config-') as directory:
    tmp = Path(directory)
    (tmp / 'Arduino.h').write_text(arduino)
    signatures = ['static String htmlEscape(', 'static String formatTime(',
                  'static String htmlHeader(', 'static String htmlFooter(', 'static String powerIcon(', 'static String dockIcon(',
                  'static void refreshEntityOptions(', 'static void handleRoot(']
    (tmp / 'test.cpp').write_text(prefix + source[source.index('static String g_cachedEntityOptions;'):source.index('static void refreshEntityOptions(')] + ''.join(function(s) for s in signatures) + tests)
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', f'-I{tmp}',
                    f'-I{ROOT/"include"}', str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test'), *sys.argv[1:]], check=True)

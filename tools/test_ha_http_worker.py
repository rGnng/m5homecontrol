#!/usr/bin/env python3
"""Exercise real HTTP worker/command code with stalled HTTP and a live main loop."""
import ast
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/ha_client.cpp').read_text()
main = (ROOT / 'src/main.cpp').read_text()


def function(text, signature):
    start = text.index(signature)
    pos = text.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


module = ast.parse((ROOT / 'tools/test_controls.py').read_text())
arduino = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'arduino' for t in n.targets))
arduino = arduino.replace('    void toUpperCase()',
                         '    bool startsWith(const char* prefix) const {return rfind(prefix,0)==0;}\n'
                         '    void toUpperCase()')
prefix = r'''
#include "ha_client.h"
#include "connected_power.h"
#include <ArduinoJson.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
void convertFromJson(JsonVariantConst source,String& out){out=source.as<std::string>();}
static auto boot=std::chrono::steady_clock::now();
static std::thread::id mainThread;
static uint32_t millis(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-boot).count();}
static void delay(int ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
struct Queue {std::mutex mutex;std::condition_variable ready;void* item=nullptr;};
using QueueHandle_t=Queue*;
static constexpr int pdTRUE=1,pdPASS=1,portMAX_DELAY=-1;
static bool queueAvailable=true,taskAvailable=true,sendAllowed=true;
static int deletedQueues=0;
static QueueHandle_t xQueueCreate(int,size_t){return queueAvailable?new Queue:nullptr;}
static void vQueueDelete(QueueHandle_t q){delete q;++deletedQueues;}
static int xQueueReceive(QueueHandle_t q,void* out,int){
    std::unique_lock<std::mutex> lock(q->mutex);q->ready.wait(lock,[&]{return q->item!=nullptr;});
    *static_cast<void**>(out)=q->item;q->item=nullptr;return pdTRUE;
}
static int xQueueSend(QueueHandle_t q,const void* in,int){
    std::lock_guard<std::mutex> lock(q->mutex);
    if(!sendAllowed||q->item)return 0;
    q->item=*static_cast<void* const*>(in);q->ready.notify_one();return pdTRUE;
}
static int xTaskCreate(void (*task)(void*),const char*,int,void*,int,void*){
    if(!taskAvailable)return 0;
    std::thread([task]{task(nullptr);}).detach();return pdPASS;
}
static bool socketConnected=true,g_haOnline=true;
static uint32_t lastPump=0;
static int pumps=0,commandAnimations=0,httpCode=200,httpDelay=100;
static bool nestedAttempt=false;
static std::function<void()> onPump;
bool haWsConnected(){return socketConnected;}
static void animateCommand(){assert(std::this_thread::get_id()==mainThread);++commandAnimations;}
static HaCommandCallback g_commandCb=animateCommand;
struct {String haUrl="http://fake",haToken="fake-token";} g_config;
static int powerWork=0,requestedRefreshes=0;
void beginConnectedPowerWork(){++powerWork;}
void endConnectedPowerWork(){--powerWork;}
static void requestEntityRefresh(const String&){++requestedRefreshes;}
static bool configComplete(){return true;}
static bool parseState(EntityState& e,JsonVariantConst data){e.state=data["state"].as<String>();return true;}
struct {template<class... T>void printf(const char*,T...){} } Serial;
struct WiFiClient : std::istringstream {
    int read(){return get();}
    bool connected(){return rdbuf()->in_avail()>0;}
};
struct WiFiClientSecure {void setInsecure(){}};
static String httpBody="{}";
struct HTTPClient {
    WiFiClient stream;
    template<class Client>void begin(Client&,const String&){}
    void setTimeout(int){} void setConnectTimeout(int){} void setReuse(bool){}
    void addHeader(const char*,const String&){}
    int GET(){assert(std::this_thread::get_id()!=mainThread);delay(httpDelay);stream.str(httpBody);return httpCode;}
    int POST(const String&){return GET();}
    String getString(){return httpBody;}
    WiFiClient& getStream(){return stream;}
    void end(){}
};
#include "deadline_stream.h"
'''
stats = source[source.index('struct HaLinkStats {'):source.index('static uint64_t clockMillis(')]
worker = source[source.index('struct HttpWork {'):source.index('static bool httpRequestBlocking(')]
signatures = ['static String escapeHtml(', 'static bool httpRequestBlocking(',
              'static bool httpRequest(', 'bool haFetchState(', 'static bool fetchEntityOptionsBlocking(',
              'bool haFetchEntityOptions(', 'bool haCallService(']
tests = r'''
void haLoop(){
    assert(std::this_thread::get_id()==mainThread);
    const uint32_t now=millis();
    if(now-lastPump>50)socketConnected=false;
    lastPump=now;++pumps;
    if(!nestedAttempt){nestedAttempt=true;assert(!runHttpWork([]{return true;}));}
    if(onPump)onPump();
}
int main(){
    mainThread=std::this_thread::get_id();
    // Resource failures return failure; they never fall back to blocking HTTP.
    queueAvailable=false;assert(!runHttpWork([]{std::abort();return false;}));
    queueAvailable=true;taskAvailable=false;
    assert(!runHttpWork([]{std::abort();return false;}) && deletedQueues==1);
    taskAvailable=true;lastPump=millis();
    assert(callService("climate","set_hvac_mode","climate.ac","\"hvac_mode\":\"cool\""));
    assert(pumps>20 && socketConnected && g_haOnline && commandAnimations==1);
    assert(powerWork==0 && requestedRefreshes==1);
    assert(g_linkStats.lastCommandStatus==200 && g_linkStats.commandFailures==0);
    assert(g_linkStats.lastCommandMs>=100);
    // An HA service error does not change connection state or stop heartbeats.
    pumps=0;httpCode=503;lastPump=millis();
    assert(!callService("climate","set_hvac_mode","climate.ac"));
    assert(pumps>20 && socketConnected && g_haOnline && commandAnimations==2);
    assert(g_linkStats.lastCommandStatus==503 && g_linkStats.commandFailures==1);
    httpCode=-11;lastPump=millis();
    assert(!callService("climate","set_temperature","climate.ac"));
    assert(socketConnected && g_haOnline && g_linkStats.lastCommandStatus==-11);
    assert(g_linkStats.commandFailures==2);
    // REST reads and the filtered entity selector also use the HTTP worker.
    httpCode=200;httpBody="{\"state\":\"cool\"}";String body;
    pumps=0;lastPump=millis();
    assert(httpRequest("GET","/api/states/climate.ac","",body));
    assert(body==httpBody && pumps>20 && commandAnimations==3);
    httpBody=R"([{"entity_id":"climate.ac","attributes":{"friendly_name":"A & B"}},{"entity_id":"light.test","attributes":{"friendly_name":"Light"}}])";
    String options;pumps=0;lastPump=millis();
    assert(haFetchEntityOptions(options,1));
    assert(options=="<option value=\"climate.ac\">A &amp; B</option>");
    assert(pumps>20 && socketConnected);
    // Keep a newer live state if an older HTTP snapshot (or timeout) follows it.
    EntityState state;state.id="climate.ac";state.available=true;
    httpBody=R"({"state":"old"})";
    onPump=[&]{state.state="new";++state.revision;};lastPump=millis();
    assert(haFetchState(state) && state.state=="new");
    httpCode=-11;lastPump=millis();
    assert(haFetchState(state) && state.state=="new" && state.available);
    onPump=nullptr;httpCode=200;lastPump=millis();
    assert(haFetchState(state) && state.state=="old");
    // Queue rejection is reported, with the last successful connection intact.
    sendAllowed=false;
    assert(!callService("climate","set_temperature","climate.ac"));
    assert(g_linkStats.lastCommandStatus==0 && g_linkStats.commandFailures==3);
    assert(socketConnected && g_haOnline && !g_httpWaiting);
    assert(powerWork==0 && requestedRefreshes==1);
    std::puts("PASS: stalled HTTP keeps WebSocket pumping; command success/errors/timeouts, callbacks, entity list, queue/resource failures, reentrancy and link state");
    std::fflush(stdout);
    // The firmware worker is intentionally permanent; end without tearing down its queue.
    std::_Exit(0);
}
'''
with tempfile.TemporaryDirectory(prefix='m5-http-worker-') as directory:
    tmp = Path(directory)
    (tmp / 'Arduino.h').write_text(arduino)
    code = prefix + stats + worker + ''.join(function(source, s) for s in signatures)
    code += function(main, 'static bool callService(') + tests
    (tmp / 'test.cpp').write_text(code)
    subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                    f'-I{tmp}', f'-I{ROOT/"include"}',
                    f'-I{ROOT/".pio/libdeps/m5stack-core/ArduinoJson/src"}',
                    str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True, timeout=10)

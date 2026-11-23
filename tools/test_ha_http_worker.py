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
#include <vector>
void convertFromJson(JsonVariantConst source,String& out){out=source.as<std::string>();}
static auto boot=std::chrono::steady_clock::now();
static std::thread::id mainThread;
static uint32_t millis(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-boot).count();}
static void delay(int ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
struct Queue {std::mutex mutex;std::condition_variable ready;void* item=nullptr;};
using QueueHandle_t=Queue*;
static constexpr int pdTRUE=1,pdPASS=1,portMAX_DELAY=-1;
static bool queueAvailable=true,taskAvailable=true,sendAllowed=true;
static std::atomic<bool> workerAllowed{true};
static int deletedQueues=0;
static QueueHandle_t xQueueCreate(int,size_t){return queueAvailable?new Queue:nullptr;}
static void vQueueDelete(QueueHandle_t q){delete q;++deletedQueues;}
static int xQueueReceive(QueueHandle_t q,void* out,int){
    std::unique_lock<std::mutex> lock(q->mutex);q->ready.wait(lock,[&]{return q->item!=nullptr;});
    *static_cast<void**>(out)=q->item;q->item=nullptr;
    while(!workerAllowed.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return pdTRUE;
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
static bool g_dataSuspended=false,g_wsResumePending=false,g_wsStarted=true;
static bool g_wsAuthed=true,g_stateSubscribed=true;
static uint32_t g_wsClockId=1,g_wsStateId=2,g_dataGeneration=0,g_discardedHttp=0;
bool haDataSuspended(){return g_dataSuspended;}
uint32_t haDataGeneration(){return g_dataGeneration;}
struct {void disconnect(){} void setReconnectInterval(int){} void setSuspended(bool){} } ws;
static void notifyStatus(bool){}
static uint32_t lastPump=0;
static int pumps=0,commandAnimations=0,httpCode=200,httpDelay=100;
bool haWsConnected(){return socketConnected;}
static void animateCommand(){assert(std::this_thread::get_id()==mainThread);++commandAnimations;}
static HaCommandCallback g_commandCb=animateCommand;
struct {String haUrl="http://fake",haToken="fake-token";int entityCount=1;} g_config;
static int powerWork=0,requestedRefreshes=0;
static constexpr int MAX_ENTITIES=1;
static bool g_dirty[MAX_ENTITIES]={};
static EntityState g_cards[MAX_ENTITIES];
void beginConnectedPowerWork(){++powerWork;}
void endConnectedPowerWork(){--powerWork;}
static void requestEntityRefresh(const String&){++requestedRefreshes;}
static bool configComplete(){return true;}
static bool parseState(EntityState& e,JsonVariantConst data){e.state=data["state"].as<String>();return true;}
struct {template<class... T>void printf(const char*,T...){} } Serial;
struct WiFiClient : std::istringstream {
    virtual ~WiFiClient()=default;
    virtual int read(){return get();}
    virtual int read(uint8_t* bytes,size_t length){size_t n=0;while(n<length && rdbuf()->in_avail()>0)bytes[n++]=get();return n?n:-1;}
    virtual int available(){return rdbuf()->in_avail();}
    virtual int peek(){return std::istringstream::peek();}
    virtual size_t write(uint8_t){return 1;}
    virtual size_t write(const uint8_t*,size_t n){return n;}
    virtual void stop(){}
    virtual uint8_t connected(){return rdbuf()->in_avail()>0;}
};
struct WiFiClientSecure : WiFiClient {void setInsecure(){}void setHandshakeTimeout(unsigned long){}};
using ResponsiveClient=WiFiClient;
using ResponsiveSecureClient=WiFiClientSecure;
static String httpBody="{}";
struct ObservedRequest {String url,authorization,body;};
static std::vector<ObservedRequest> observed;
struct HTTPClient {
    WiFiClient stream;
    String url,authorization,body;
    template<class Client>void begin(Client&,const String& value){url=value;}
    void setTimeout(int){} void setConnectTimeout(int){} void setReuse(bool){}
    void addHeader(const char* name,const String& value){if(std::string(name)=="Authorization")authorization=value;}
    int GET(){assert(std::this_thread::get_id()!=mainThread);observed.push_back({url,authorization,body});delay(httpDelay);stream.str(httpBody);return httpCode;}
    int POST(const String& value){body=value;return GET();}
    String getString(){return httpBody;}
    int getSize(){return httpBody.size();}
    WiFiClient& getStream(){return stream;}
    void end(){}
};
#include "deadline_stream.h"
#include "transfer_budget.h"
'''
stats = source[source.index('struct HaLinkStats {'):source.index('static uint64_t clockMillis(')]
worker = source[source.index('struct HttpWork {'):source.index('static bool httpRequestBlocking(')]
signatures = ['static String escapeHtml(', 'static bool httpRequestBlocking(',
              'bool haFetchState(', 'static bool fetchEntityOptionsBlocking(',
              'bool haFetchEntityOptions(', 'bool haCallService(']
tests = r'''
static int navigationTicks=0;
void haLoop(){
    assert(std::this_thread::get_id()==mainThread);
    const uint32_t now=millis();
    if(now-lastPump>50)socketConnected=false;
    lastPump=now;++pumps;
    pollHttpWork();
}
static void tick(){
    ++navigationTicks; // A normal main-loop iteration can still service input.
    haLoop();delay(1);
}
static void drain(){
    uint32_t deadline=millis()+3000;
    while(haHttpBusy()){assert(millis()<deadline);tick();}
    assert(powerWork==0);
}
int main(){
    mainThread=std::this_thread::get_id();lastPump=millis();
    auto never=[](HttpWork&){std::abort();return false;};
    queueAvailable=false;assert(!runHttpWork(never,nullptr));
    queueAvailable=true;taskAvailable=false;
    assert(!runHttpWork(never,nullptr) && deletedQueues==1);
    taskAvailable=true;
    bool completed=false,result=false;
    auto completion=[&](bool ok){
        assert(std::this_thread::get_id()==mainThread);completed=true;result=ok;
    };
    uint32_t started=millis();
    assert(callService("climate","set_hvac_mode","climate.ac","\"hvac_mode\":\"cool\"",completion));
    assert(millis()-started<50 && !completed && haHttpBusy());
    assert(commandAnimations==0); // LED starts at dispatch, not submission.
    drain();
    assert(completed && result && navigationTicks>20 && pumps>20);
    assert(socketConnected && g_haOnline && commandAnimations==1 && requestedRefreshes==1);
    assert(g_linkStats.lastCommandStatus==200 && g_linkStats.lastCommandMs>=100);
    // Service and transport failures complete asynchronously without changing link state.
    for(int status:{503,-11}){
        completed=false;httpCode=status;
        assert(callService("climate","set_temperature","climate.ac","",completion));
        assert(!completed);drain();assert(completed && !result);
        assert(g_linkStats.lastCommandStatus==status && socketConnected && g_haOnline);
    }
    assert(g_linkStats.commandFailures==2);
    // Commands queue behind a slow read, ahead of any new background work.
    httpCode=200;httpBody=R"({"state":"old"})";
    EntityState state;state.id="climate.ac";state.available=true;
    completed=false;assert(haFetchState(state,completion));tick();
    assert(!haFetchState(state,completion));
    assert(haCallService("switch","toggle","switch.a","",nullptr));
    assert(!haCallService("switch","toggle","switch.a","",nullptr));
    state.state="new";++state.revision;
    drain();assert(state.state=="new" && completed && result);
    // A newer WebSocket update also wins over a failed read.
    httpCode=-11;completed=false;assert(haFetchState(state,completion));
    state.state="live";++state.revision;drain();
    assert(result && state.state=="live" && state.available);
    httpCode=200;assert(haFetchState(state,completion));drain();assert(state.state=="old");
    // Request values are owned snapshots, not borrowed caller/config Strings.
    std::vector<String> requests;
    for(unsigned i=0;i<HTTP_WORK_LIMIT;++i){
        String id="switch."+String(i);
        assert(haCallService("switch","toggle",id,"",[&,id](bool ok){assert(ok);requests.push_back(id);}));
    }
    assert(!haCallService("switch","toggle","switch.overflow"));
    drain();assert(requests.size()==HTTP_WORK_LIMIT);
    for(unsigned i=0;i<HTTP_WORK_LIMIT;++i)assert(requests[i]=="switch."+String(i));
    {
        String domain="switch",service="toggle",id="switch.snapshot";
        assert(haCallService(domain,service,id));
        domain=service=id="changed";
        g_config.haUrl="http://changed";g_config.haToken="changed";
    }
    drain();
    assert(observed.back().url=="http://fake/api/services/switch/toggle");
    assert(observed.back().authorization=="Bearer fake-token");
    assert(observed.back().body==R"({"entity_id":"switch.snapshot"})");
    g_config.haUrl="http://fake";g_config.haToken="fake-token";
    // Completion can safely enqueue new work after its request slot is released.
    bool nested=false;
    assert(haCallService("switch","toggle","switch.a","",[&](bool ok){
        assert(ok);nested=haCallService("switch","toggle","switch.a");
    }));drain();assert(nested);
    // Filtered entity options are delivered after returning to the caller.
    httpBody=R"([{"entity_id":"climate.ac","attributes":{"friendly_name":"A & B"}}])";
    String options;assert(haFetchEntityOptions([&](bool ok,const String& value){assert(ok);options=value;},1));
    assert(options.isEmpty());drain();
    assert(options=="<option value=\"climate.ac\">A &amp; B</option>");
    // Suspend cancels queued commands, drains an active read without applying
    // its result, and rejects new work. Waking cannot revive an old response.
    state.state="kept";completed=false;
    httpBody=R"({"state":"stale"})";
    assert(haFetchState(state,completion));tick();
    bool cancelled=false;
    assert(haCallService("switch","toggle","switch.cancel","",[&](bool ok){assert(!ok);cancelled=true;}));
    delay(2);size_t sentBefore=observed.size();
    haSetDataSuspended(true);
    assert(cancelled && !g_wsAuthed && !g_stateSubscribed && powerWork==0);
    assert(!haFetchState(state,completion) && !haCallService("switch","toggle","switch.a"));
    assert(!haFetchEntityOptions(nullptr));
    haSetDataSuspended(false);drain();
    assert(completed && !result && state.state=="kept" && observed.size()==sentBefore);
    assert(g_discardedHttp==2);
    // Late options cannot repopulate a paused selector.
    bool optionsCancelled=false;
    assert(haFetchEntityOptions([&](bool ok,const String&){assert(!ok);optionsCancelled=true;}));tick();
    haSetDataSuspended(true);drain();assert(optionsCancelled);
    haSetDataSuspended(false);
    // A dispatched request whose task has not started yet is cancellable too.
    workerAllowed.store(false);completed=false;
    auto requestCount=observed.size();
    assert(haCallService("switch","toggle","switch.notstarted","",completion));tick();
    haSetDataSuspended(true);workerAllowed.store(true);drain();
    assert(completed && !result && observed.size()==requestCount);
    haSetDataSuspended(false);
    // Dispatch failure completes once and releases the work/power guard.
    sendAllowed=false;completed=false;
    assert(haCallService("switch","toggle","switch.a","",completion));
    drain();assert(completed && !result && g_linkStats.lastCommandStatus==0);
    assert(g_linkStats.commandFailures==3 && socketConnected && g_haOnline);
    assert(powerWork==0 && !haHttpBusy() && g_dirty[0]==false);
    std::puts("PASS: nonblocking reads/commands/options, navigation during slow HTTP, revision races, bounded FIFO, duplicate rejection, callback enqueue, errors and resource cleanup");
    std::fflush(stdout);
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

#!/usr/bin/env python3
"""Test actual HA clock WebSocket handlers with captured transport messages."""
import ast
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/ha_client.cpp').read_text()
def function(signature):
    start=source.index(signature);pos=source.index('{',start);depth=1;end=pos+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]+'\n'
# Reuse the small Arduino String host stub without running the controls suite.
module=ast.parse((ROOT/'tools/test_controls.py').read_text())
arduino=next(ast.literal_eval(n.value) for n in module.body if isinstance(n,ast.Assign)
             and any(isinstance(t,ast.Name) and t.id=='arduino' for t in n.targets))
prefix=r'''
#include "ha_client.h"
#include "ha_clock.h"
#include <ArduinoJson.h>
#include <cassert>
#include <vector>
void convertFromJson(JsonVariantConst source,String& out){out=source.as<std::string>();}
static uint64_t nowMs=1000;
static uint32_t millis(){return static_cast<uint32_t>(nowMs);}
static int64_t esp_timer_get_time(){return nowMs*1000;}
struct {template<class... T>void printf(const char*,T...){} void println(const char*){}} Serial;
static std::vector<std::string> sent;
static void pollHttpWork(){}
static bool g_dataSuspended=false,g_wsResumePending=false;
static uint32_t g_dataGeneration=0,g_discardedHttp=0;
struct {int loops=0;void loop(){++loops;} void disconnect(){} void setReconnectInterval(int){} } ws;
enum WStype_t {WStype_CONNECTED,WStype_DISCONNECTED,WStype_TEXT,WStype_ERROR};
static constexpr int WL_CONNECTED=3;
struct {int status(){return WL_CONNECTED;}} WiFi;
static bool g_wsStarted=true,g_wsAuthed=false;
static uint32_t g_wsMessageId=1,g_wsClockId=0,g_clockRetryAt=0;
static uint32_t g_wsStateId=0,g_stateRetryAt=0;
static bool g_stateSubscribed=false;
static unsigned g_watchedEntities=0;
static HaClock g_haClock;
static String g_haTimeZone;
static void (*g_authCb)()=nullptr;
static int stateEvents=0;
static void onState(const EntityState&){++stateEvents;}
static HaStateCallback g_stateCb=onState;
struct {String haToken="fake-host-test-token";int entityCount=3;
String entities[3]={"light.test","climate.ac","light.test"};
String auxEntities[3]={"sensor.temp","light.test",""};}g_config;
static bool lastOnline=false;
static void notifyStatus(bool online){lastOnline=online;}
static bool parseState(EntityState&,JsonVariantConst){return true;}
static void wsSendJson(JsonDocument& doc){std::string out;serializeJson(doc,out);sent.push_back(out);}
'''
signatures=['static uint64_t clockMillis(', 'bool haClockReady(', 'bool haGetLocalTime(', 'String haTimeZone(',
            'String haLocalTimeText(', 'String haClockStatus(', 'String haConnectionStatus(', 'static void wsSubscribeClock(',
            'static bool syncClock(', 'static bool stateEntityConfigured(', 'static void wsSubscribe(', 'static void wsHandleText(', 'static void wsEvent(',
            'void haLoop(', 'bool haWsConnected(', 'bool haStateUpdatesReady(']
stats=source[source.index('struct HaLinkStats {'):source.index('static uint64_t clockMillis(')]
tests=r'''
static void message(const std::string& json){wsHandleText(reinterpret_cast<const uint8_t*>(json.data()),json.size());}
static void clockEvent(uint32_t id,int64_t epoch,int offset,const char* zone,bool encoded=false){
    JsonDocument sample;sample["epoch"]=epoch;sample["utc_offset"]=offset;sample["time_zone"]=zone;
    JsonDocument event;event["id"]=id;event["type"]="event";
    if(encoded){std::string value;serializeJson(sample,value);event["event"]["result"]=value;}
    else event["event"]["result"]=sample.as<JsonVariantConst>();
    std::string json;serializeJson(event,json);message(json);
}
int main(){
    g_dataSuspended=true;int loops=ws.loops;
    haLoop();assert(ws.loops==loops && sent.empty());
    message(R"({"type":"auth_required"})");assert(sent.empty());
    g_dataSuspended=false;
    assert(!haClockReady() && haLocalTimeText()=="Unavailable");
    wsEvent(WStype_CONNECTED,nullptr,0);
    assert(g_linkStats.connections==1 && g_linkStats.lastConnectMs==1000 && !lastOnline);
    message(R"({"type":"auth_required"})");
    JsonDocument request;assert(!deserializeJson(request,sent.back()));assert(request["type"]=="auth");
    message(R"({"type":"auth_ok"})");
    assert(g_wsAuthed && sent.size()==3 && g_wsClockId!=0);
    assert(lastOnline && g_linkStats.authentications==1 && g_linkStats.lastAuthMs==1000);
    assert(!deserializeJson(request,sent.back()));assert(request["type"]=="render_template");
    assert(request["template"].as<std::string>().find("now()")!=std::string::npos);
    assert(!haStateUpdatesReady() && g_watchedEntities==3);
    assert(!deserializeJson(request,sent[1]));
    assert(request["type"]=="subscribe_trigger" && request["trigger"]["platform"]=="state");
    assert(request["trigger"]["entity_id"].size()==3);
    uint32_t stateSubscription=g_wsStateId;
    message("{\"type\":\"result\",\"success\":true,\"id\":"+std::to_string(stateSubscription)+"}");
    assert(haStateUpdatesReady());
    uint32_t subscription=g_wsClockId;
    // Both forms are used by HA versions: a JSON object or JSON encoded text.
    clockEvent(subscription,1767225600,28800,"Asia/Shanghai");
    assert(haClockReady());
    assert(haTimeZone()=="Asia/Shanghai" && haLocalTimeText()=="2026-01-01T08:00:00");
    nowMs+=61000;assert(haLocalTimeText()=="2026-01-01T08:01:01");
    clockEvent(subscription,1767225660,-18000,"America/New_York",true);
    assert(haTimeZone()=="America/New_York" && haLocalTimeText()=="2025-12-31T19:01:00");
    std::string status=haClockStatus();assert(status.find("time_source=ha")!=std::string::npos);
    assert(status.find("utc_offset=-18000")!=std::string::npos);
    clockEvent(subscription+100,1767225660,3600,"Wrong/OldSubscription");
    assert(haTimeZone()=="America/New_York");
    message(R"({"id":1,"type":"event","event":{"variables":{"trigger":{"entity_id":"light.test","to_state":{"entity_id":"light.test","state":"on"}}}}})");
    assert(stateEvents==1);
    clockEvent(subscription,0,0,"Invalid/Time");assert(haTimeZone()=="America/New_York");
    size_t count=sent.size();nowMs+=29999;haLoop();assert(sent.size()==count);
    ++nowMs;haLoop();assert(sent.size()==count+2 && g_wsClockId!=subscription);
    assert(!deserializeJson(request,sent[count]));assert(request["type"]=="unsubscribe_events");
    assert(request["subscription"].as<uint32_t>()==subscription);
    subscription=g_wsClockId;
    message("{\"type\":\"result\",\"success\":false,\"id\":"+std::to_string(subscription)+"}");
    assert(g_wsClockId==0);nowMs+=30000;count=sent.size();haLoop();assert(sent.size()==count+1);
    clockEvent(g_wsClockId,1767225660,20700,"Asia/Kathmandu");
    assert(haLocalTimeText()=="2026-01-01T05:46:00");
    nowMs+=180000;count=sent.size();haLoop();assert(sent.size()==count+2);
    // Reauthentication creates a fresh subscription while the last clock keeps running.
    subscription=g_wsClockId;message(R"({"type":"auth_ok"})");assert(g_wsClockId!=subscription);
    assert(g_haClock.ready());clockEvent(g_wsClockId,1767225840,0,"UTC");
    assert(haTimeZone()=="UTC" && haLocalTimeText()=="2026-01-01T00:04:00");
    nowMs+=500;
    wsEvent(WStype_ERROR,nullptr,0);assert(g_linkStats.errors==1);
    String disconnectedAt=haLocalTimeText();
    wsEvent(WStype_DISCONNECTED,nullptr,0);
    assert(!lastOnline && !haWsConnected() && g_wsClockId==0 && !haStateUpdatesReady() && g_wsStateId==0);
    assert(g_linkStats.disconnects==1 && g_linkStats.lastDisconnectMs==nowMs);
    assert(g_linkStats.lastDisconnectTime==disconnectedAt);
    status=haConnectionStatus();
    assert(status.find("wifi_connected=1")!=std::string::npos && status.find("ws_connected=0")!=std::string::npos);
    assert(status.find("ws_disconnects=1")!=std::string::npos);
    wsEvent(WStype_CONNECTED,nullptr,0);message(R"({"type":"auth_ok"})");
    assert(lastOnline && haWsConnected() && g_linkStats.connections==2);
    assert(g_linkStats.authentications==3 && g_linkStats.disconnects==1);
    haLoop();nowMs+=25;haLoop();assert(g_linkStats.maxLoopGapMs>=25);
    // Rejects unrelated/stale messages; retries subscription errors and missing acknowledgments.
    stateSubscription=g_wsStateId;
    message("{\"type\":\"result\",\"success\":false,\"id\":"+std::to_string(stateSubscription)+"}");
    assert(!haStateUpdatesReady() && g_wsStateId==0 && g_linkStats.subscriptionErrors==1);
    count=sent.size();nowMs+=29999;haLoop();assert(sent.size()==count);
    ++nowMs;haLoop();assert(g_wsStateId && g_wsStateId!=stateSubscription);
    stateSubscription=g_wsStateId;
    nowMs+=30000;count=sent.size();haLoop();assert(sent.size()==count+2 && g_wsStateId!=stateSubscription);
    message("{\"type\":\"result\",\"success\":true,\"id\":"+std::to_string(stateSubscription)+"}");
    assert(!haStateUpdatesReady());
    message("{\"type\":\"result\",\"success\":true,\"id\":"+std::to_string(g_wsStateId)+"}");
    assert(haStateUpdatesReady());
    auto stateEvent=[&](uint32_t id,const char* entity,const char* toState){
        JsonDocument d;d["id"]=id;d["type"]="event";
        d["event"]["variables"]["trigger"]["entity_id"]=entity;
        if(toState){d["event"]["variables"]["trigger"]["to_state"]["entity_id"]=toState;}
        std::string json;serializeJson(d,json);message(json);
    };
    stateEvent(stateSubscription,"light.test","light.test");
    stateEvent(g_wsStateId,"light.unconfigured","light.unconfigured");
    stateEvent(g_wsStateId,"light.test","climate.ac");
    assert(stateEvents==1);
    stateEvent(g_wsStateId,"sensor.temp","sensor.temp");
    stateEvent(g_wsStateId,"climate.ac",nullptr);
    assert(stateEvents==3 && g_linkStats.stateEvents==3);
    g_config.entityCount=0;wsSubscribe();
    assert(g_wsStateId==0 && g_watchedEntities==0 && haStateUpdatesReady());
    std::puts("PASS: actual HA clock subscription, object/text updates, local time/zone diagnostics, invalid/stale IDs, state events, retries and reconnects");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-clock-ws-') as directory:
    tmp=Path(directory);(tmp/'Arduino.h').write_text(arduino)
    (tmp/'test.cpp').write_text(prefix+stats+''.join(function(s) for s in signatures)+tests)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{tmp}',f'-I{ROOT/"include"}',f'-I{ROOT/".pio/libdeps/m5stack-core/ArduinoJson/src"}',str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)

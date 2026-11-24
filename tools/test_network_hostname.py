#!/usr/bin/env python3
"""Exercise the actual network lifecycle using delayed SDK events and a fake LAN."""
import ast
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/app_config.cpp').read_text()
module = ast.parse((ROOT / 'tools/test_controls.py').read_text())
arduino = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'arduino' for t in n.targets))
arduino = arduino.replace('    void toUpperCase()', '''    bool endsWith(const char* text)const{auto n=std::char_traits<char>::length(text);return size()>=n && compare(size()-n,n,text)==0;}
    void remove(size_t from){erase(from);}
    String substring(size_t from,size_t to)const{return substr(from,to-from);}
    void toUpperCase()''')

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
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
AppConfig g_config;
using arduino_event_id_t = int;
enum { ARDUINO_EVENT_WIFI_STA_START, ARDUINO_EVENT_WIFI_STA_STOP, ARDUINO_EVENT_WIFI_STA_CONNECTED,
       ARDUINO_EVENT_WIFI_STA_DISCONNECTED, ARDUINO_EVENT_WIFI_STA_GOT_IP, ARDUINO_EVENT_WIFI_STA_LOST_IP,
       ARDUINO_EVENT_WIFI_AP_START, ARDUINO_EVENT_WIFI_AP_STOP };
struct arduino_event_info_t {
    struct { unsigned reason; } wifi_sta_disconnected;
    struct { struct { struct { uint32_t addr; } ip; } ip_info; } got_ip;
};
struct StaticQueue_t { unsigned capacity=0, size=0; std::deque<std::vector<uint8_t>> events; };
using QueueHandle_t = StaticQueue_t*;
static constexpr int pdTRUE=1;
static QueueHandle_t xQueueCreateStatic(unsigned capacity,unsigned size,uint8_t*,StaticQueue_t* queue){
    queue->capacity=capacity;queue->size=size;return queue;
}
static int xQueueSend(QueueHandle_t queue,const void* data,int){
    if(queue->events.size()==queue->capacity)return 0;
    auto bytes=static_cast<const uint8_t*>(data);queue->events.emplace_back(bytes,bytes+queue->size);return pdTRUE;
}
static int xQueueReceive(QueueHandle_t queue,void* data,int){
    if(queue->events.empty())return 0;
    memcpy(data,queue->events.front().data(),queue->size);queue->events.pop_front();return pdTRUE;
}
static void xQueueReset(QueueHandle_t queue){queue->events.clear();}
static constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_ARG=1,WIFI_STA=1,WIFI_AP=2,WIFI_IF_STA=1;
static constexpr int WIFI_FAST_SCAN=0,WIFI_CONNECT_AP_BY_SIGNAL=0,WIFI_AUTH_OPEN=0,WIFI_AUTH_WPA2_PSK=3,WIFI_PS_NONE=0;
struct esp_netif_t {String hostname="esp32-default";bool up=false;};
static esp_netif_t netif;
static bool netifPresent=true,hostnameReadOk=true;
static int requestedTxPower=0, configWrites=0, starts=0, stops=0, connects=0, disconnects=0;
static bool radio=false, associated=false, failStart=false, failStop=false;
static uint32_t ip=0,now=100;
static uint32_t millis(){return now;}
static esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key){
    assert(!std::strcmp(key,"WIFI_STA_DEF") || !std::strcmp(key,"WIFI_AP_DEF"));return netifPresent?&netif:nullptr;
}
static int esp_netif_get_hostname(esp_netif_t* station,const char** name){
    assert(station==&netif);*name=station->hostname.c_str();return hostnameReadOk?ESP_OK:ESP_FAIL;
}
static bool esp_netif_is_netif_up(esp_netif_t* iface){return iface->up;}
struct wifi_config_t {struct {
    uint8_t ssid[32],password[64];int listen_interval,scan_method,sort_method;
    struct {int rssi,authmode;} threshold;struct {bool capable;} pmf_cfg;
} sta;};
static wifi_config_t radioConfig;
static int esp_wifi_set_config(int mode,const wifi_config_t* config){
    assert(mode==WIFI_IF_STA && radio);radioConfig=*config;++configWrites;return ESP_OK;
}
static int esp_wifi_set_max_tx_power(int value){assert(radio);requestedTxPower=value;return ESP_OK;}
static void esp_wifi_set_ps(int ps){assert(radio && ps==WIFI_PS_NONE);}
struct wifi_ap_record_t{};
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t*){return associated?ESP_OK:ESP_FAIL;}
static int esp_wifi_start(){assert(!radio);++starts;if(failStart)return ESP_FAIL;radio=true;return ESP_OK;}
static int esp_wifi_stop(){++stops;if(failStop)return ESP_FAIL;radio=associated=false;return ESP_OK;}
static int esp_wifi_disconnect(){assert(radio);++disconnects;associated=false;return ESP_OK;}
static int esp_wifi_connect(){assert(radio && configWrites);++connects;return ESP_OK;}
struct FakeWiFi {
    String cachedName;
    int coldStarts=0, modeValue=0;
    void (*event)(arduino_event_id_t,arduino_event_info_t)=nullptr;
    void onEvent(decltype(event) callback){assert(!event);event=callback;}
    void setAutoReconnect(bool enabled){assert(!enabled);}
    void setSleep(bool enabled){assert(!enabled);}
    void setHostname(const char* name){cachedName=name;}
    bool mode(int value){++coldStarts;modeValue=value;netif.hostname=cachedName;return esp_wifi_start()==ESP_OK;}
    bool softAP(const char* ssid){assert(modeValue==WIFI_AP && !std::strcmp(ssid,"HA-Remote"));return true;}
    struct IP {uint32_t address;explicit operator uint32_t() const{return address;}String toString(){return "192.0.2.10";}};
    IP localIP(){return {ip};}
};
static FakeWiFi WiFi;
static bool g_apMode=false,g_mdnsStarted=false,g_serverStarted=false,g_networkSuspended=false;
static unsigned serviceStarts=0,serviceStops=0,haStops=0;
static struct {void handleClient(){}void stopServing(){++serviceStops;}} server;
static struct {bool begin(const char*){return true;}void addService(const char*,const char*,int){}void end(){}} MDNS;
static void startWebServer(){if(!g_serverStarted)++serviceStarts;g_serverStarted=true;}
static void haNetworkDown(){++haStops;}
struct {template<class... T>void printf(const char*,T...){} void println(const char*){}}Serial;
'''

tests = r'''
static void emit(arduino_event_id_t id,uint32_t detail=0){
    if(id==ARDUINO_EVENT_WIFI_STA_CONNECTED) {associated=true;netif.up=true;}
    if(id==ARDUINO_EVENT_WIFI_STA_DISCONNECTED || id==ARDUINO_EVENT_WIFI_STA_STOP || id==ARDUINO_EVENT_WIFI_AP_STOP){
        associated=false;netif.up=false;
    }
    if(id==ARDUINO_EVENT_WIFI_STA_GOT_IP)ip=detail;
    arduino_event_info_t info{};
    info.wifi_sta_disconnected.reason=detail;
    info.got_ip.ip_info.ip.addr=detail;
    assert(WiFi.event);WiFi.event(id,info);
}
static void ready(uint32_t addr=123){
    emit(ARDUINO_EVENT_WIFI_STA_START);networkLoop();
    assert(!networkConnected() && !g_serverStarted);
    emit(ARDUINO_EVENT_WIFI_STA_CONNECTED);networkLoop();
    assert(!networkConnected() && !g_serverStarted);
    emit(ARDUINO_EVENT_WIFI_STA_GOT_IP,addr);networkLoop();
    assert(networkConnected() && g_serverStarted && g_mdnsStarted);
}
int main(){
    const auto cleaned=sanitizeHostname(String(32,'a'));
    assert(cleaned.size()==31 && sanitizeHostname("  My Remote.local  ")=="myremote");
    assert(sanitizeHostname("---")=="ha-remote");
    g_config.ssid="test";g_config.pass="test-pass";g_config.hostname=cleaned;
    // The ghost dock wake begins offline without allocating or starting Wi-Fi.
    assert(networkSetSuspended(true));networkLoop();assert(!radio && !WiFi.event);
    networkSetSuspended(false);networkLoop();assert(radio && connects==0 && stationHostname()==cleaned);
    ready();
    assert(configWrites==1 && std::strcmp(reinterpret_cast<char*>(radioConfig.sta.ssid),"test")==0);
    assert(radioConfig.sta.listen_interval==10 && requestedTxPower==80);
    assert(g_wifiIpEvents==1 && serviceStarts==1 && connects==1);
    // A polled-ready client loses every old socket on a rejoin between loop ticks.
    emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,200);
    emit(ARDUINO_EVENT_WIFI_STA_CONNECTED);
    emit(ARDUINO_EVENT_WIFI_STA_GOT_IP,123);
    networkLoop();
    assert(networkConnected() && g_wifiIpEvents==2 && serviceStops==1 && haStops==1 && serviceStarts==2);
    // Repeated GOT_IP for the same lease must not interrupt users. An address change must.
    emit(ARDUINO_EVENT_WIFI_STA_GOT_IP,123);networkLoop();assert(serviceStarts==2);
    emit(ARDUINO_EVENT_WIFI_STA_GOT_IP,124);networkLoop();assert(serviceStarts==3 && serviceStops==2 && haStops==2);
    // Sleep waits for the disconnect and stop events, and a wake arriving early
    // waits for that same shutdown before starting the station again.
    assert(!networkSetSuspended(true));networkLoop();
    assert(!networkConnected() && !g_serverStarted && disconnects==1 && stops==0);
    emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,8);networkLoop();
    assert(!networkSetSuspended(true) && stops==1 && !radio);
    networkSetSuspended(false);networkLoop();assert(starts==1);
    emit(ARDUINO_EVENT_WIFI_STA_STOP);networkLoop();assert(starts==2 && connects==1);
    emit(ARDUINO_EVENT_WIFI_STA_GOT_IP,124);networkLoop();assert(!networkConnected());
    ready(124);assert(WiFi.coldStarts==1 && configWrites==1 && connects==2);
    // Actual sleep leaves Wi-Fi stopped through health ticks until wake.
    networkSetSuspended(true);networkLoop();
    emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,8);networkLoop();
    emit(ARDUINO_EVENT_WIFI_STA_STOP);networkLoop();assert(networkSetSuspended(true));
    now+=60000;networkLoop();assert(!radio && starts==2);
    networkSetSuspended(false);networkLoop();ready(124);assert(g_wifiStopEvents==2 && configWrites==1);
    // DHCP never arrives even though the driver can still report the old address.
    emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,200);networkLoop();
    now+=WIFI_RETRY_MS;networkLoop();
    emit(ARDUINO_EVENT_WIFI_STA_CONNECTED);networkLoop();
    assert(!networkConnected() && !g_serverStarted && ip==124);
    now+=WIFI_IP_TIMEOUT_MS;networkLoop();assert(g_wifiRadioRestarts==1);
    emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,8);networkLoop();
    emit(ARDUINO_EVENT_WIFI_STA_STOP);networkLoop();assert(!radio);
    now+=WIFI_RETRY_MS;networkLoop();ready();
    // A failed stop cannot allow sleep. A successful stop without an event
    // can complete if the network interface has already gone down.
    failStop=true;networkSetSuspended(true);networkLoop();
    emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,8);networkLoop();assert(!networkSetSuspended(true) && radio);
    failStop=false;now+=WIFI_EVENT_TIMEOUT_MS;networkLoop();assert(!radio && !networkSetSuspended(true));
    now+=WIFI_EVENT_TIMEOUT_MS;networkLoop();assert(networkSetSuspended(true));
    // Retry a failed radio start and preserve unsigned deadlines over rollover.
    now=UINT32_MAX-1000;failStart=true;networkBegin();assert(!radio && g_wifiConnectError==ESP_FAIL);
    networkLoop();assert(!radio);
    failStart=false;now+=WIFI_RETRY_MS;networkLoop();ready();
    // Event loss causes one controlled restart with old services closed.
    for(unsigned i=0;i<WIFI_EVENT_QUEUE_SIZE+1;++i)emit(ARDUINO_EVENT_WIFI_STA_GOT_IP,123);
    networkLoop();assert(!networkConnected() && !g_serverStarted && g_wifiRadioRestarts==2);
    emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,8);networkLoop();
    emit(ARDUINO_EVENT_WIFI_STA_STOP);networkLoop();
    now+=WIFI_RETRY_MS;networkLoop();ready();
    // An unconfigured device still exposes its setup AP after cold and warm starts.
    networkSetSuspended(true);networkLoop();
    emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,8);networkLoop();
    emit(ARDUINO_EVENT_WIFI_STA_STOP);networkLoop();
    g_wifiInitialized=false;g_config.ssid="";
    networkBegin();assert(g_apMode && !g_serverStarted);
    emit(ARDUINO_EVENT_WIFI_AP_START);networkLoop();assert(g_serverStarted && !networkConnected());
    networkSetSuspended(true);networkLoop();emit(ARDUINO_EVENT_WIFI_AP_STOP);networkLoop();
    assert(networkSetSuspended(true) && !g_serverStarted);networkSetSuspended(false);networkLoop();
    emit(ARDUINO_EVENT_WIFI_AP_START);networkLoop();assert(g_serverStarted && !networkConnected());
    netifPresent=false;assert(stationHostname()=="unavailable");
    netifPresent=true;hostnameReadOk=false;assert(stationHostname()=="unavailable");
    std::puts("PASS: live lifecycle code with delayed events, fresh IP, socket refresh, sleep/wake, DHCP timeout, stop/start errors, queue loss, rollover and setup AP");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-network-lifecycle-') as directory:
    tmp = Path(directory)
    (tmp / 'Arduino.h').write_text(arduino)
    constants = source[source.index('static const char* DEFAULT_HOSTNAME'):source.index('\n\nAppConfig g_config;')]
    state = source[source.index('enum class WifiPhase'):source.index('static String sanitizeHostname')]
    network = source[source.index('static String stationHostname() {'):source.index('bool configModeActive() {')]
    code = prefix + constants + '\n' + state + function('static String sanitizeHostname(') + network + tests
    (tmp / 'test.cpp').write_text(code)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', f'-I{tmp}',
                    f'-I{ROOT / "include"}', str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)

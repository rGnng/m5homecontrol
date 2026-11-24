#!/usr/bin/env python3
"""Reproduce the pinned Arduino core's cached/STA hostname startup behavior."""
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
#include <cstring>
#include <vector>
AppConfig g_config;
static constexpr int ESP_OK=0,WIFI_OFF=0,WIFI_STA=1,WIFI_IF_STA=1,WL_CONNECTED=3;
struct esp_netif_t {String hostname="esp32-D75ED0";};
static esp_netif_t netif;
static bool netifPresent=true,hostnameReadOk=true;
static esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key){assert(!std::strcmp(key,"WIFI_STA_DEF"));return netifPresent?&netif:nullptr;}
static int esp_netif_get_hostname(esp_netif_t* interface,const char** name){assert(interface==&netif);*name=interface->hostname.c_str();return hostnameReadOk?ESP_OK:-1;}
struct wifi_config_t {struct {int listen_interval=3;}sta;};
static wifi_config_t radioConfig;
static int requestedTxPower=0,txPowerRequests=0;
static int esp_wifi_set_max_tx_power(int value){requestedTxPower=value;++txPowerRequests;return ESP_OK;}
static int esp_wifi_get_config(int mode,wifi_config_t* out){assert(mode==WIFI_IF_STA);*out=radioConfig;return ESP_OK;}
static int esp_wifi_set_config(int mode,const wifi_config_t* in){assert(mode==WIFI_IF_STA);radioConfig=*in;return ESP_OK;}
static std::vector<String> offeredHostnames;
using arduino_event_id_t = int;
static constexpr int ARDUINO_EVENT_WIFI_STA_DISCONNECTED = 4;
struct arduino_event_info_t {struct {unsigned reason;} wifi_sta_disconnected;};
struct wifi_ap_record_t {};
struct FakeWiFi {
    String cache="esp32-D75ED0";
    bool started=false,online=false,associated=false,hasIp=false,connectFails=false;
    int starts=0;
    inline static void (*eventCallback)(arduino_event_id_t, arduino_event_info_t)=nullptr;
    void onEvent(decltype(eventCallback) callback,int event){assert(event==ARDUINO_EVENT_WIFI_STA_DISCONNECTED);eventCallback=callback;}
    bool setHostname(const char* name){cache=String(name).substr(0,31);return true;}
    const char* getHostname(){return cache.c_str();}
    bool mode(int value){if(value==WIFI_OFF){started=online=associated=hasIp=false;return true;}assert(value==WIFI_STA);if(!started){netif.hostname=cache;++starts;}started=true;return true;}
    void setSleep(bool enabled){assert(!enabled);}
    void begin(const char*,const char*,int,const void*,bool connect){assert(started && !connect);radioConfig.sta.listen_interval=3;}
    void connect(){assert(started);offeredHostnames.push_back(netif.hostname);if(!connectFails)online=associated=hasIp=true;}
    void begin(){if(status()!=WL_CONNECTED)connect();}
    void disconnect(){online=associated=hasIp=false;}
    int status(){return online?WL_CONNECTED:0;}
    String SSID(){return "test";}
    int RSSI(){return -65;}
    struct IP {bool valid;explicit operator uint32_t() const{return valid?1:0;}String toString(){return "192.0.2.10";}};
    IP localIP(){return {hasIp};}

};
static FakeWiFi WiFi;
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t*){return WiFi.associated?ESP_OK:-1;}
static int esp_wifi_disconnect(){WiFi.disconnect();return ESP_OK;}
static int esp_wifi_connect(){WiFi.connect();return WiFi.connectFails?-1:ESP_OK;}
static bool g_apMode=false,apStarted=false,g_networkSuspended=false;
static bool g_wifiWasConnected=false,g_mdnsStarted=false,g_serverStarted=true;
static struct {int calls=0,stops=0;void handleClient(){++calls;}void stop(){++stops;}struct Client{void stop(){}};Client client(){return {};}}server;
static struct {bool begin(const char*){return true;}void addService(const char*,const char*,int){}void end(){}}MDNS;
static void startWebServer(){g_serverStarted=true;}
static uint32_t g_wifiCheckedAt=0,g_lastReconnect=0,g_wifiRadioRestarts=0,now=100;
static unsigned g_wifiRetryCount=0;
static int g_wifiConnectError=ESP_OK;
static std::atomic<unsigned> g_wifiDisconnectReason{0};
static uint32_t millis(){return now;}
static void startAccessPoint(){apStarted=true;}
struct {template<class... T>void printf(const char*,T...){} void println(const char*){}}Serial;
'''
tests = r'''
int main(){
    // Core 2.x caches a late setHostname() without changing the DHCP interface.
    WiFi.mode(WIFI_STA);WiFi.setHostname("desired-name");WiFi.begin();
    assert(String(WiFi.getHostname())=="desired-name");
    assert(stationHostname()=="esp32-D75ED0" && offeredHostnames.back()=="esp32-D75ED0");
    for(const char* name:{"ha-remote","bedroom-controller"}){
        WiFi=FakeWiFi{};netif=esp_netif_t{};offeredHostnames.clear();
        g_wifiWasConnected=g_mdnsStarted=false;txPowerRequests=0;
        g_config.ssid="test";g_config.hostname=name;
        networkBegin();
        assert(stationHostname()==name && offeredHostnames.size()==1 && offeredHostnames[0]==name);
        assert(radioConfig.sta.listen_interval==10 && !g_apMode && g_lastReconnect==now);
        assert(requestedTxPower==80 && txPowerRequests==1);
        networkLoop();assert(txPowerRequests==2 && g_wifiWasConnected);
        networkLoop();assert(txPowerRequests==2); // Do not reconfigure on every loop.
        WiFi.disconnect();now+=WIFI_CHECK_INTERVAL_MS;networkLoop();assert(!g_wifiWasConnected);
        WiFi.begin();now+=WIFI_CHECK_INTERVAL_MS;networkLoop();assert(txPowerRequests==3 && g_wifiWasConnected);
        assert(offeredHostnames.back()==name && radioConfig.sta.listen_interval==10);
    }
    // Off must stop the radio and services, remain offline through health ticks,
    // then reapply DHCP/listen configuration and reopen web/mDNS after input.
    assert(networkSetSuspended(true) && networkSuspended());
    assert(!WiFi.started && !g_serverStarted && !g_mdnsStarted && !networkConnected());
    int calls=server.calls,starts=WiFi.starts,stops=server.stops;
    networkLoop();networkLoop();assert(server.calls==calls && WiFi.starts==starts);
    networkSetSuspended(true);assert(server.stops==stops);
    // A late STA_STOP/STA_START event can leave the pre-sleep status visible
    // during wake. Arduino begin() uses that cache to decide whether to join.
    WiFi.online=true;
    const auto joinsBeforeWake=offeredHostnames.size();
    WiFi.begin();
    assert(offeredHostnames.size()==joinsBeforeWake && !WiFi.associated);
    assert(networkSetSuspended(false) && !networkSuspended());
    assert(WiFi.started && radioConfig.sta.listen_interval==10);
    assert(offeredHostnames.size()==joinsBeforeWake+1 && WiFi.associated && WiFi.hasIp);
    assert(offeredHostnames.back()==g_config.hostname);
    networkLoop();assert(g_serverStarted && g_mdnsStarted && networkConnected());
    networkSetSuspended(false);assert(WiFi.starts==starts+1);
    assert(WiFi.eventCallback);
    WiFi.eventCallback(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, {{2}});
    assert(g_wifiDisconnectReason.load()==2);
    // Cached status can stay connected even though association and IP are gone.
    now+=60000;
    WiFi.associated=WiFi.hasIp=false;
    const auto beforeRetry=offeredHostnames.size();
    networkLoop();
    assert(!networkConnected() && WiFi.status()==WL_CONNECTED);
    assert(offeredHostnames.size()==beforeRetry); // Give auto-reconnect a grace period.
    now+=WIFI_RETRY_INTERVAL_MS;networkLoop();
    assert(offeredHostnames.size()==beforeRetry+1);
    now+=WIFI_CHECK_INTERVAL_MS;networkLoop();assert(networkConnected());

    // An associated station without an IP needs a new association for DHCP.
    WiFi.hasIp=false;
    now+=WIFI_CHECK_INTERVAL_MS;networkLoop();assert(!networkConnected());
    now+=WIFI_RETRY_INTERVAL_MS;networkLoop();
    now+=WIFI_CHECK_INTERVAL_MS;networkLoop();assert(networkConnected());

    // Persistent failure escalates to restarting the radio and its services.
    WiFi.connectFails=true;WiFi.disconnect();
    now+=WIFI_CHECK_INTERVAL_MS;networkLoop();
    now+=WIFI_RETRY_INTERVAL_MS;networkLoop();assert(g_wifiRetryCount==1 && g_wifiConnectError==-1);
    now+=WIFI_RETRY_INTERVAL_MS;networkLoop();assert(g_wifiRetryCount==2);
    WiFi.connectFails=false;
    now+=WIFI_RETRY_INTERVAL_MS;networkLoop();
    assert(g_wifiRadioRestarts==1 && !g_serverStarted && !g_mdnsStarted && g_wifiRetryCount==0);
    networkLoop();assert(networkConnected() && g_serverStarted && g_mdnsStarted);
    assert(radioConfig.sta.listen_interval==10 && stationHostname()==g_config.hostname);

    // The retry deadline also works across the millis() wrap.
    now=UINT32_MAX-1000;WiFi.associated=false;networkLoop();
    now+=WIFI_RETRY_INTERVAL_MS;networkLoop();
    now+=WIFI_CHECK_INTERVAL_MS;networkLoop();assert(networkConnected());
    // The application's name fits the core's terminating 32-byte cache.
    String longName(32,'a');auto cleaned=sanitizeHostname(longName);
    assert(cleaned.size()==31);
    WiFi=FakeWiFi{};g_config.hostname=cleaned;networkBegin();assert(stationHostname()==cleaned);
    assert(sanitizeHostname("  My Remote.local  ")=="myremote");
    assert(sanitizeHostname("---")=="ha-remote");
    netifPresent=false;assert(stationHostname()=="unavailable");
    netifPresent=true;hostnameReadOk=false;assert(stationHostname()=="unavailable");
    hostnameReadOk=true;g_config.ssid="";apStarted=false;networkBegin();assert(apStarted);
    std::puts("PASS: default-hostname regression, configured DHCP hostname at association/reconnect, listen interval preservation, actual link verification, DHCP recovery, radio restart and rollover");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-hostname-') as directory:
    tmp = Path(directory)
    (tmp / 'Arduino.h').write_text(arduino)
    constants = source[source.index('static const char* DEFAULT_HOSTNAME'):source.index('\n\nAppConfig g_config;')]
    timing = '\n'.join(line for line in source.splitlines() if line.startswith('static constexpr uint32_t WIFI_'))
    code = prefix + constants + timing + '\n' + ''.join(function(s) for s in ['static String sanitizeHostname(',
                                                'static String stationHostname() {', 'static void setMaximumWifiTxPower(', 'static void stopNetworkServices(', 'void networkBegin(', 'void networkLoop(', 'bool networkSuspended(', 'bool networkConnected(', 'bool networkSetSuspended(']) + tests
    (tmp / 'test.cpp').write_text(code)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', f'-I{tmp}',
                    f'-I{ROOT / "include"}', str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)

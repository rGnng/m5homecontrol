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
struct FakeWiFi {
    String cache="esp32-D75ED0";
    bool started=false,online=false;int starts=0;
    bool setHostname(const char* name){cache=String(name).substr(0,31);return true;}
    const char* getHostname(){return cache.c_str();}
    bool mode(int value){if(value==WIFI_OFF){started=online=false;return true;}assert(value==WIFI_STA);if(!started){netif.hostname=cache;++starts;}started=true;return true;}
    void setSleep(bool enabled){assert(!enabled);}
    void begin(const char*,const char*,int,const void*,bool connect){assert(started && !connect);radioConfig.sta.listen_interval=3;}
    void begin(){assert(started);offeredHostnames.push_back(netif.hostname);online=true;}
    void disconnect(){online=false;}
    int status(){return online?WL_CONNECTED:0;}
    String SSID(){return "test";}
    int RSSI(){return -65;}
    struct IP {String toString(){return "192.0.2.10";}};
    IP localIP(){return {};}

};
static FakeWiFi WiFi;
static bool g_apMode=false,apStarted=false,g_networkSuspended=false;
static bool g_wifiWasConnected=false,g_staEverConnected=false,g_mdnsStarted=false,g_serverStarted=true;
static struct {int calls=0,stops=0;void handleClient(){++calls;}void stop(){++stops;}struct Client{void stop(){}};Client client(){return {};}}server;
static struct {bool begin(const char*){return true;}void addService(const char*,const char*,int){}void end(){}}MDNS;
static void startWebServer(){g_serverStarted=true;}
static uint32_t g_connectStart=0,g_lastReconnect=0;
static uint32_t millis(){return 100;}
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
        g_wifiWasConnected=g_staEverConnected=g_mdnsStarted=false;txPowerRequests=0;
        g_config.ssid="test";g_config.hostname=name;
        networkBegin();
        assert(stationHostname()==name && offeredHostnames.size()==1 && offeredHostnames[0]==name);
        assert(radioConfig.sta.listen_interval==10 && !g_apMode && g_connectStart==100);
        assert(requestedTxPower==80 && txPowerRequests==1);
        networkLoop();assert(txPowerRequests==2 && g_wifiWasConnected);
        networkLoop();assert(txPowerRequests==2); // Do not reconfigure on every loop.
        WiFi.disconnect();networkLoop();assert(!g_wifiWasConnected);
        WiFi.begin();networkLoop();assert(txPowerRequests==3 && g_wifiWasConnected);
        assert(offeredHostnames.back()==name && radioConfig.sta.listen_interval==10);
    }
    // Off must stop the radio and services, remain offline through health ticks,
    // then reapply DHCP/listen configuration and reopen web/mDNS after input.
    assert(networkSetSuspended(true) && networkSuspended());
    assert(!WiFi.started && !g_serverStarted && !g_mdnsStarted && !g_staEverConnected);
    int calls=server.calls,starts=WiFi.starts,stops=server.stops;
    networkLoop();networkLoop();assert(server.calls==calls && WiFi.starts==starts);
    networkSetSuspended(true);assert(server.stops==stops);
    assert(networkSetSuspended(false) && !networkSuspended());
    assert(WiFi.started && radioConfig.sta.listen_interval==10);
    assert(offeredHostnames.back()==g_config.hostname);
    networkLoop();assert(g_serverStarted && g_mdnsStarted && g_staEverConnected);
    networkSetSuspended(false);assert(WiFi.starts==starts+1);
    // The application's name fits the core's terminating 32-byte cache.
    String longName(32,'a');auto cleaned=sanitizeHostname(longName);
    assert(cleaned.size()==31);
    WiFi=FakeWiFi{};g_config.hostname=cleaned;networkBegin();assert(stationHostname()==cleaned);
    assert(sanitizeHostname("  My Remote.local  ")=="myremote");
    assert(sanitizeHostname("---")=="ha-remote");
    netifPresent=false;assert(stationHostname()=="unavailable");
    netifPresent=true;hostnameReadOk=false;assert(stationHostname()=="unavailable");
    hostnameReadOk=true;g_config.ssid="";apStarted=false;networkBegin();assert(apStarted);
    std::puts("PASS: default-hostname regression, configured DHCP hostname at association/reconnect, listen interval preservation, name length and actual netif diagnostics");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-hostname-') as directory:
    tmp = Path(directory)
    (tmp / 'Arduino.h').write_text(arduino)
    constants = source[source.index('static const char* DEFAULT_HOSTNAME'):source.index('\n\nAppConfig g_config;')]
    code = prefix + constants + '\n' + ''.join(function(s) for s in ['static String sanitizeHostname(',
                                                'static String stationHostname() {', 'static void setMaximumWifiTxPower(', 'void networkBegin(', 'void networkLoop(', 'bool networkSuspended(', 'bool networkSetSuspended(']) + tests
    (tmp / 'test.cpp').write_text(code)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', f'-I{tmp}',
                    f'-I{ROOT / "include"}', str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)

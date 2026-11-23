#!/usr/bin/env python3
"""Run the actual async connection owner with a blocked worker and main-loop callbacks."""
from pathlib import Path
import subprocess
import tempfile
from test_network_io import ROOT, write_headers

EXTRA = {
    'freertos/FreeRTOS.h': r'''
#pragma once
#include <condition_variable>
#include <mutex>
#include <thread>
constexpr int pdPASS=1,pdTRUE=1;constexpr unsigned portMAX_DELAY=~0u;
inline bool failQueue=false,failTask=false,failSend=false;
struct Queue{std::mutex mutex;std::condition_variable ready;void* item=nullptr;};
using QueueHandle_t=Queue*;
inline QueueHandle_t xQueueCreate(int,size_t){return failQueue?nullptr:new Queue;}
inline void vQueueDelete(QueueHandle_t q){delete q;}
inline int xQueueSend(QueueHandle_t q,void* item,unsigned){
    if(failSend)return 0;
    std::lock_guard<std::mutex> lock(q->mutex);if(q->item)return 0;
    q->item=*static_cast<void**>(item);q->ready.notify_one();return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t q,void* item,unsigned){
    std::unique_lock<std::mutex> lock(q->mutex);q->ready.wait(lock,[q]{return q->item;});
    *static_cast<void**>(item)=q->item;q->item=nullptr;return pdTRUE;
}
inline int xTaskCreate(void(*fn)(void*),const char*,unsigned,void* argument,int,void*){
    if(failTask)return 0;std::thread(fn,argument).detach();return pdPASS;
}
''',
    'freertos/queue.h': '#pragma once\n#include "FreeRTOS.h"\n',
    'freertos/task.h': '#pragma once\n#include "FreeRTOS.h"\n',
    'WebSocketsClient.h': r'''
#pragma once
#include <WiFiClientSecure.h>
#include <cassert>
constexpr int WSC_NOT_CONNECTED=0,WSC_HEADER=1,WSC_CONNECTED=2;
inline std::thread::id mainThread;
inline int callbacks=0,frames=0,heartbeats=0;inline bool closeDuringRead=false;
class WebSocketsClient{
protected:
    struct Client{WiFiClient* tcp=nullptr;WiFiClientSecure* ssl=nullptr;bool isSSL=false;int status=0;} _client;
    String _host="mock";uint16_t _port=0;uint32_t _lastConnectionFail=0,_reconnectInterval=50;
    bool clientIsConnected(Client* c){
        if(c->tcp && c->tcp->connected() && c->status!=WSC_NOT_CONNECTED)return true;
        delete c->tcp;c->tcp=nullptr;c->ssl=nullptr;c->status=WSC_NOT_CONNECTED;return false;
    }
    void connectedCb(){assert(std::this_thread::get_id()==mainThread);++callbacks;_client.status=WSC_HEADER;}
    void handleClientData(){
        assert(std::this_thread::get_id()==mainThread);++frames;
        if(closeDuringRead){disconnect();return;}_client.status=WSC_CONNECTED;
    }
    void handleHBPing(){assert(std::this_thread::get_id()==mainThread);++heartbeats;}
    void handleHBTimeout(Client*){}
public:
    void begin(bool tls=false){_port=8123;_client.isSSL=tls;}
    void disconnect(){delete _client.tcp;_client.tcp=nullptr;_client.ssl=nullptr;_client.status=0;}
    bool online(){return _client.tcp && _client.tcp->connected();}
    WiFiClient* socket(){return _client.tcp;}
};
''',
}

TEST=r'''
#include "async_websocket.h"
#include <cstdio>
#include <cstdlib>
static int guards=0;
void beginConnectedPowerWork(){assert(std::this_thread::get_id()==mainThread);++guards;}
void endConnectedPowerWork(){assert(std::this_thread::get_id()==mainThread);assert(guards>0);--guards;}
template<class F>void until(F done){
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!done()){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
}
int main(){
    mainThread=std::this_thread::get_id();now=100;
    auto* ws=new AsyncWebSocket;ws->begin();
    releaseConnect=false;ws->loop();until([]{return connectEntered.load();});
    assert(guards==1 && connectThread!=mainThread && connectTimeout==1500);
    // A worker stalled in connect cannot block UI/input/sleep-state processing.
    for(int i=0;i<10000;++i)ws->loop();assert(guards==1 && callbacks==0);
    releaseConnect=true;until([&]{ws->pollConnect();return guards==0;});
    assert(ws->online() && callbacks==1 && ws->socket()->streamTimeout==100);
    ws->loop();assert(frames==1 && heartbeats==1);
    // Remote disconnect during parsing must not trigger a synchronous base reconnect.
    closeDuringRead=true;ws->loop();assert(!ws->online() && guards==0);closeDuringRead=false;
    now=200;releaseConnect=false;connectEntered=false;ws->loop();until([]{return connectEntered.load();});
    ws->setSuspended(true);releaseConnect=true;
    // Drain a cancelled connect even while Off/Dim suspends normal HA polling.
    until([&]{ws->pollConnect();return guards==0;});
    assert(!ws->online() && clients==0 && callbacks==1);
    for(int i=0;i<100;++i)ws->loop();assert(guards==0);
    // Resuming starts exactly one fresh job; no cancelled socket is adopted.
    ws->setSuspended(false);now=300;ws->loop();until([&]{ws->pollConnect();return guards==0;});
    assert(ws->online() && callbacks==2 && clients==1);ws->disconnect();
    // Failed connections retry after the configured delay, with balanced guards.
    connectResult=false;now=400;ws->loop();until([&]{ws->pollConnect();return guards==0;});
    assert(!ws->online() && clients==0);ws->loop();assert(guards==0);
    now=451;connectResult=true;ws->loop();until([&]{ws->pollConnect();return guards==0;});
    assert(ws->online());ws->disconnect();
    // Allocation/queue errors do not leak the power guard or lock out later retries.
    auto* failing=new AsyncWebSocket;failing->begin();now=600;failQueue=true;failing->loop();assert(guards==0);
    failQueue=false;failTask=true;now=700;failing->loop();assert(guards==0);
    failTask=false;failSend=true;now=800;failing->loop();assert(guards==0);
    failSend=false;now=900;failing->loop();until([&]{failing->pollConnect();return guards==0;});
    assert(failing->online());failing->disconnect();
    // TLS uses a finite handshake and socket-write wait, with callbacks still on main.
    auto* tls=new AsyncWebSocket;tls->begin(true);now=1000;tls->loop();
    until([&]{tls->pollConnect();return guards==0;});
    assert(tls->online() && insecure && handshakeTimeout==2);
    assert(static_cast<WiFiClientSecure*>(tls->socket())->socketTimeout()==250);tls->disconnect();
    assert(clients==0 && guards==0);
    puts("PASS: stalled connect, main-loop responsiveness, cancellation, resume, retry, allocation errors, TLS and power-guard balance");
    fflush(stdout);std::_Exit(0); // Embedded tasks intentionally live for the process lifetime.
}
'''
with tempfile.TemporaryDirectory(prefix='m5-async-ws-') as directory:
    tmp=Path(directory);write_headers(tmp)
    for name,content in EXTRA.items():
        path=tmp/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content)
    (tmp/'test.cpp').write_text(TEST)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pthread',f'-I{tmp}',f'-I{ROOT/"include"}',str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True,timeout=10)

#!/usr/bin/env python3
"""Exercise the actual bounded socket writes with backpressure and short writes."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADERS = {
    'Arduino.h': r'''
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <chrono>
using String=std::string;
inline std::atomic<uint32_t> now{0};
inline uint32_t millis(){return now.load();}
inline void delay(unsigned ms){now.fetch_add(ms);std::this_thread::yield();}
''',
    'WiFi.h': r'''
#pragma once
#include <Arduino.h>
inline std::atomic<bool> connectEntered{false},releaseConnect{true},connectResult{true};
inline std::thread::id connectThread;
inline std::atomic<int> clients{0},connectTimeout{0};
class Stream {public:unsigned streamTimeout=0;void setTimeout(unsigned ms){streamTimeout=ms;}};
class WiFiClient:public Stream{
public:
    bool open=true;int socket=1;unsigned timeout=0;
    WiFiClient(){++clients;}virtual ~WiFiClient(){--clients;}
    virtual int connect(const char*,uint16_t,int ms){
        connectTimeout=ms;connectThread=std::this_thread::get_id();connectEntered=true;
        while(!releaseConnect.load())std::this_thread::yield();
        open=connectResult;return open;
    }
    int fd(){return socket;}
    virtual int setTimeout(uint32_t seconds){timeout=seconds;return 0;}
    virtual void stop(){open=false;}
    virtual uint8_t connected(){return open;}
    virtual int available(){return 0;}
    virtual int peek(){return -1;}
    virtual int read(){return -1;}
    virtual int read(uint8_t*,size_t){return -1;}
    virtual size_t write(uint8_t){return 1;}
    virtual size_t write(const uint8_t*,size_t n){return n;}
};
''',
    'WiFiClientSecure.h': r'''
#pragma once
#include <WiFi.h>
inline unsigned handshakeTimeout=0;inline bool insecure=false;
class WiFiClientSecure:public WiFiClient{
protected:
    struct SSL{unsigned socket_timeout=0;} context;
    SSL* sslclient=&context;
public:
    int setTimeout(uint32_t seconds)override{timeout=seconds;context.socket_timeout=seconds*1000;return 0;}
    void setHandshakeTimeout(unsigned seconds){handshakeTimeout=seconds;}
    void setInsecure(){insecure=true;}
    unsigned socketTimeout()const{return context.socket_timeout;}
};
''',
    'lwip/sockets.h': r'''
#pragma once
#include <cstddef>
#include <cerrno>
#include <algorithm>
constexpr int MSG_DONTWAIT=0x40;
inline int sendMode=0,sendCalls=0;
inline size_t sentBytes=0;
inline int send(int fd,const void*,size_t n,int flags){
    if(flags!=MSG_DONTWAIT || fd<0)__builtin_abort();
    ++sendCalls;
    if(sendMode==1 || (sendMode==3 && sendCalls==1)){errno=EAGAIN;return -1;}
    if(sendMode==2){errno=ECONNRESET;return -1;}
    if(sendMode==4 && sendCalls==1){errno=EINTR;return -1;}
    size_t count=std::min<size_t>(n,3);sentBytes+=count;return count;
}
''',
}

def write_headers(tmp):
    for name, content in HEADERS.items():
        path=tmp/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content)

TEST = r'''
#include "network_io.h"
#include <cassert>
#include <cstdio>
int main(){
    uint8_t bytes[20]{};
    assert(writeSocketWithin(1,bytes,20)==20 && sendCalls==7);
    now=0;sendMode=1;assert(writeSocketWithin(1,bytes,20)==0 && millis()==250);
    now=UINT32_MAX-49;assert(writeSocketWithin(1,bytes,20)==0 && millis()==200);
    now=0;sendMode=2;assert(writeSocketWithin(1,bytes,20)==0 && millis()==0);
    sendMode=3;sendCalls=0;assert(writeSocketWithin(1,bytes,20)==20 && millis()==1);
    sendMode=4;sendCalls=0;assert(writeSocketWithin(1,bytes,20)==20 && millis()==2);
    assert(writeSocketWithin(-1,bytes,20)==0);
    ResponsiveClient c;sendMode=1;assert(c.write(bytes,20)==0 && !c.open);
    c.open=true;sendMode=0;assert(c.write(uint8_t(7))==1 && c.open);
    ResponsiveSecureClient tls;tls.setTimeout(8);assert(tls.socketTimeout()==250 && tls.timeout==8);
    puts("PASS: partial writes, backpressure, close, EINTR, deadline rollover and TLS write timeout");
}
'''

if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='m5-network-io-') as directory:
        tmp=Path(directory);write_headers(tmp);(tmp/'test.cpp').write_text(TEST)
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',f'-I{tmp}',f'-I{ROOT/"include"}',str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
        subprocess.run([str(tmp/'test')],check=True,timeout=5)

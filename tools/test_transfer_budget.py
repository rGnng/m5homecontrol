#!/usr/bin/env python3
"""Exercise the real Arduino HTTP body/chunk readers through BudgetClient."""
import ast
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
module=ast.parse((ROOT/'tools/test_controls.py').read_text())
arduino=next(ast.literal_eval(n.value) for n in module.body if isinstance(n,ast.Assign)
             and any(isinstance(t,ast.Name) and t.id=='arduino' for t in n.targets))
arduino=arduino.replace('    bool isEmpty()', '    size_t length()const{return size();}\n    operator const char*()const{return c_str();}\n    bool isEmpty()')
source=(Path.home()/'.platformio/packages/framework-arduinoespressif32/libraries/HTTPClient/src/HTTPClient.cpp').read_text()
def function(signature):
    a=source.index(signature);b=source.index('{',a)+1;depth=1
    while depth:depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]+'\n'
prefix=r'''
#include <Arduino.h>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cstring>
static uint32_t now=0,cancelAt=UINT32_MAX;
static std::atomic<bool>* cancelFlag=nullptr;
uint32_t millis(){return now;}
void delay(unsigned ms){now+=ms;if(cancelFlag && now>=cancelAt)cancelFlag->store(true);}
#include "transfer_budget.h"
#define log_d(...) do{}while(0)
#define log_w(...) do{}while(0)
constexpr int HTTP_TCP_BUFFER_SIZE=1460,HTTPC_ERROR_STREAM_WRITE=-10,HTTPC_ERROR_TOO_LESS_RAM=-8,
    HTTPC_ERROR_NO_STREAM=-6,HTTPC_ERROR_NOT_CONNECTED=-4,HTTPC_ERROR_CONNECTION_LOST=-5,
    HTTPC_ERROR_READ_TIMEOUT=-11,HTTPC_ERROR_ENCODING=-9,HTTPC_TE_IDENTITY=0,HTTPC_TE_CHUNKED=1;
struct Client{
    String payload;size_t position=0,chunk=1460;uint32_t gap=0,next=0;bool open=true,closeAtEnd=false;
    virtual ~Client()=default;
    virtual void stop(){open=false;}
    virtual uint8_t connected(){return open && !(closeAtEnd && position==payload.size());}
    virtual int available(){return open && now>=next?std::min(chunk,payload.size()-position):0;}
    virtual int peek(){return available()?static_cast<unsigned char>(payload[position]):-1;}
    virtual int read(){uint8_t value;return read(&value,1)==1?value:-1;}
    virtual int read(uint8_t* out,size_t n){size_t count=std::min<size_t>(n,available());if(!count)return -1;memcpy(out,payload.data()+position,count);position+=count;next=now+gap;return count;}
    virtual size_t write(uint8_t){return 1;}
    virtual size_t write(const uint8_t*,size_t n){return n;}
    size_t readBytes(uint8_t* out,size_t n){size_t count=0;uint32_t start=now;while(count<n && now-start<200){int c=read();if(c<0){if(!connected())break;delay(1);}else out[count++]=c;}return count;}
    String readStringUntil(char delimiter){String out;uint32_t start=now;while(now-start<200){int c=read();if(c<0){if(!connected())break;delay(1);}else if(c==delimiter)break;else out+=static_cast<char>(c);}return out;}
};
struct Stream{
    String bytes;
    size_t write(const uint8_t* data,size_t n){bytes.append(reinterpret_cast<const char*>(data),n);return n;}
    bool getWriteError(){return false;}void clearWriteError(){}
};
struct HTTPClient{
    Client* _client;int _size,_transferEncoding;
    HTTPClient(Client& c,int n,int encoding=HTTPC_TE_IDENTITY):_client(&c),_size(n),_transferEncoding(encoding){}
    bool connected(){return _client->available()>0 || _client->connected();}
    int returnError(int error){_client->stop();return error;}
    void disconnect(bool){_client->stop();}
    int writeToStreamDataBlock(Stream*,int);int writeToStream(Stream*);
};
'''
tests=r'''
int main(){
    {TransferBudget b(100,64);BudgetClient<Client> c(b);c.payload="{\"state\":\"on\"}";Stream out;HTTPClient h(c,c.payload.size());
     assert(h.writeToStream(&out)==int(c.payload.size())&&out.bytes==c.payload&&!b.failed());}
    // Known size, silently stalled connection. It must end despite connected()==true.
    {now=0;TransferBudget b(100,64);BudgetClient<Client> c(b);c.payload="partial";Stream out;HTTPClient h(c,30);
     assert(h.writeToStream(&out)<0 && now==100 && !c.open && b.failed());}
    // Unknown length/trickling peers must not extend the total budget.
    {now=0;TransferBudget b(100,64);BudgetClient<Client> c(b);c.payload=String(200,'x');c.chunk=1;c.gap=20;Stream out;HTTPClient h(c,-1);
     h.writeToStream(&out);assert(now==100 && !c.open && b.failed() && out.bytes.size()==5);}
    // Suspension cancels a body that is already being read.
    {now=0;std::atomic<bool> cancelled{false};cancelFlag=&cancelled;cancelAt=20;
     TransferBudget b(1000,1024,&cancelled);BudgetClient<Client> c(b);c.payload=String(100,'x');c.chunk=1;c.gap=5;Stream out;HTTPClient h(c,100);
     assert(h.writeToStream(&out)<0 && now==20 && b.failed());cancelFlag=nullptr;}
    // Read() and read(buffer) count bytes once, rather than once per virtual dispatch.
    {now=0;TransferBudget b(100,3);BudgetClient<Client> c(b);c.payload="abcd";uint8_t tail[2];
     assert(c.read()=='a' && c.read(tail,2)==2 && b.allowed());assert(c.read()=='d' && !b.allowed());}
    // Exact chunked decoder, including empty/truncated headers and trailing CRLF.
    {now=0;TransferBudget b(100,128);BudgetClient<Client> c(b);c.payload="3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n";
     Stream out;HTTPClient h(c,-1,HTTPC_TE_CHUNKED);assert(h.writeToStream(&out)==5&&out.bytes=="abcde"&&!b.failed());}
    {now=0;TransferBudget b(100,128);BudgetClient<Client> c(b);c.payload="5\r\nabc";
     Stream out;HTTPClient h(c,-1,HTTPC_TE_CHUNKED);assert(h.writeToStream(&out)<0 && b.failed());}
    {now=0;TransferBudget b(100,128);BudgetClient<Client> c(b);c.payload="3\r\nabcXX";
     Stream out;HTTPClient h(c,-1,HTTPC_TE_CHUNKED);assert(h.writeToStream(&out)<0);}
    // Memory cap is independent of length headers, and deadlines survive millis rollover.
    {now=0;TransferBudget b(100,8);BudgetClient<Client> c(b);c.payload=String(100,'x');c.chunk=1;Stream out;HTTPClient h(c,-1);
     h.writeToStream(&out);assert(b.failed() && out.bytes.size()<=9);}
    {now=UINT32_MAX-49;TransferBudget b(100,64);BudgetClient<Client> c(b);Stream out;HTTPClient h(c,1);
     assert(h.writeToStream(&out)<0 && now==50 && b.failed());}
    puts("PASS: real HTTP/chunk decoder, full bodies, silent/trickling peers, cancellation, byte caps, EOF, malformed chunks and rollover");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-transfer-budget-') as directory:
    tmp=Path(directory);(tmp/'Arduino.h').write_text(arduino)
    (tmp/'test.cpp').write_text(prefix+function('int HTTPClient::writeToStreamDataBlock(')+function('int HTTPClient::writeToStream(')+tests)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{tmp}',f'-I{ROOT/"include"}',str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True,timeout=5)

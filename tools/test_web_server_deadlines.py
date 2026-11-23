#!/usr/bin/env python3
"""Run the actual guarded Arduino request/multipart parser on simulated sockets."""
from pathlib import Path
import subprocess
import tempfile
from prepare_webserver import guarded_sources

ROOT = Path(__file__).resolve().parents[1]
SDK = Path.home()/'.platformio/packages/framework-arduinoespressif32'
header, source = guarded_sources((SDK/'libraries/WebServer/src/WebServer.h').read_text(),
                                  (SDK/'libraries/WebServer/src/Parsing.cpp').read_text())

def function(signature):
    a=source.index(signature);b=source.index('{',a)+1;depth=1
    while depth:
        depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]+'\n'

prefix=r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#define F(x) x
#define FPSTR(x) x
#define log_v(...) do{}while(0)
#define log_e(...) do{}while(0)
#define log_w(...) do{}while(0)
class String:public std::string{
public:
    using std::string::string;using std::string::operator=;
    String()=default;String(const std::string& s):std::string(s){}
    unsigned length()const{return size();}
    char charAt(unsigned i)const{return i<size()?at(i):0;}
    String substring(unsigned a,unsigned b=UINT32_MAX)const{
        if(a>b)std::swap(a,b);if(a>=size())return "";return substr(a,std::min<unsigned>(b,size())-a);
    }
    int indexOf(char c,unsigned a=0)const{auto p=find(c,a);return p==npos?-1:int(p);}
    int indexOf(const char* s,unsigned a=0)const{auto p=find(s,a);return p==npos?-1:int(p);}
    int indexOf(const String& s,unsigned a=0)const{return indexOf(s.c_str(),a);}
    bool startsWith(const char* s)const{return rfind(s,0)==0;}
    bool startsWith(const String& s)const{return startsWith(s.c_str());}
    void trim(){auto a=find_first_not_of(" \t\r\n");if(a==npos){clear();return;}*this=substr(a,find_last_not_of(" \t\r\n")-a+1);}
    bool equalsIgnoreCase(const String& s)const{if(size()!=s.size())return false;for(unsigned i=0;i<size();++i)if(std::tolower(at(i))!=std::tolower(s[i]))return false;return true;}
    void replace(const char* from,const char* to){size_t p=0,n=std::strlen(from);while((p=find(from,p))!=npos){std::string::replace(p,n,to);p+=std::strlen(to);}}
    long toInt()const{return std::strtol(c_str(),nullptr,10);}
};
static uint32_t now=0;
uint32_t millis(){return now;}
void delay(unsigned ms){now+=ms;assert(now<1000000);}
struct WiFiClient{
    std::string input;size_t position=0,chunk=512,trickleAfter=SIZE_MAX;uint32_t gap=0,next=0;bool open=true;
    explicit WiFiClient(std::string bytes):input(std::move(bytes)){}
    int available(){return open && now>=next?std::min(position>=trickleAfter?size_t(1):chunk,input.size()-position):0;}
    bool connected(){return open;}
    int read(){if(!available())return -1;unsigned char c=input[position++];if(position>=trickleAfter || position%chunk==0)next=now+gap;return c;}
    size_t readBytes(char* out,size_t n){size_t i=0;while(i<n){int c=read();if(c<0)break;out[i++]=c;}return i;}
    void stop(){open=false;}
    void flush(){}
};
enum HTTPMethod{HTTP_GET,HTTP_HEAD,HTTP_POST,HTTP_PUT,HTTP_PATCH,HTTP_DELETE,HTTP_OPTIONS,HTTP_ANY};
static const char* _http_method_str[]={"GET","HEAD","POST","PUT","PATCH","DELETE","OPTIONS"};
static const char* Content_Type="Content-Type",*filename="filename";
constexpr int HTTP_MAX_POST_WAIT=5000,HTTP_RAW_BUFLEN=1436,HTTP_UPLOAD_BUFLEN=1436,WEBSERVER_MAX_POST_ARGS=32;
enum HTTPUploadStatus{UPLOAD_FILE_START,UPLOAD_FILE_WRITE,UPLOAD_FILE_END,UPLOAD_FILE_ABORTED};
enum HTTPRawStatus{RAW_START,RAW_WRITE,RAW_END,RAW_ABORTED};
struct HTTPUpload{HTTPUploadStatus status;String name,filename,type;size_t totalSize=0,currentSize=0;uint8_t buf[HTTP_UPLOAD_BUFLEN];};
struct HTTPRaw{HTTPRawStatus status;size_t totalSize=0,currentSize=0;char buf[HTTP_RAW_BUFLEN];};
namespace mime{enum{txt};struct Mime{const char* mimeType;};static Mime mimeTable[]={{"text/plain"}};}
class WebServer;
struct RequestHandler{
    std::vector<uint8_t> bytes;int starts=0,ends=0,aborts=0;
    RequestHandler* next(){return nullptr;}
    bool canHandle(HTTPMethod,const String&){return true;}
    bool canRaw(const String&){return false;}
    bool canUpload(const String& uri){return uri=="/update";}
    void raw(WebServer&,const String&,HTTPRaw&){}
    void upload(WebServer&,const String&,HTTPUpload& upload){
        if(upload.status==UPLOAD_FILE_START)++starts;
        if(upload.status==UPLOAD_FILE_END)++ends;
        if(upload.status==UPLOAD_FILE_ABORTED)++aborts;
        if(upload.status==UPLOAD_FILE_WRITE)bytes.insert(bytes.end(),upload.buf,upload.buf+upload.currentSize);
    }
};
class WebServer{
public:
    struct RequestArgument{String key,value;};
    struct Header{String key,value;};
    Header _currentHeaders[1];int _headerKeysCount=0;
    String _currentUri,_hostHeader;HTTPMethod _currentMethod=HTTP_ANY;int _currentVersion=0;
    bool _chunked=false;size_t _clientContentLength=0;
    RequestArgument *_currentArgs=nullptr,*_postArgs=nullptr;int _currentArgCount=0,_postArgsLen=0;
    RequestHandler handler,*_firstHandler=&handler,*_currentHandler=nullptr;
    std::unique_ptr<HTTPUpload> _currentUpload;std::unique_ptr<HTTPRaw> _currentRaw;
    uint32_t _requestReadStarted=0,_requestReadLimit=5000;bool _requestReadTimedOut=false;
    ~WebServer(){delete[] _currentArgs;delete[] _postArgs;}
    bool _parseRequest(WiFiClient&);bool _parseForm(WiFiClient&,String,uint32_t);
    void _parseArguments(String);bool _collectHeader(const char*,const char*);
    String _readStringUntil(WiFiClient&,char);int _uploadReadByte(WiFiClient&);
    void _uploadWriteByte(uint8_t);bool _parseFormUploadAborted();static String urlDecode(const String&);
    String arg(const String& name){for(int i=0;i<_currentArgCount;++i)if(_currentArgs[i].key==name)return _currentArgs[i].value;return "";}
    bool hasArg(const String& name){for(int i=0;i<_currentArgCount;++i)if(_currentArgs[i].key==name)return true;return false;}
};
'''
tests=r'''
static std::string request(const std::string& path,const std::string& type,const std::string& body,size_t declared=0){
    return "POST "+path+" HTTP/1.1\r\nHost: device\r\nContent-Type: "+type+"\r\nContent-Length: "+std::to_string(declared?declared:body.size())+"\r\n\r\n"+body;
}
static std::string multipart(const std::string& bytes,bool complete=true){
    std::string out="--m5boundary\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"firmware.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n"+bytes;
    if(complete)out+="\r\n--m5boundary--\r\n";
    return out;
}
int main(){
    {WebServer s;WiFiClient c("GET /diag HTTP/1.1\r\nHost: device\r\n\r\n");assert(s._parseRequest(c)&&s._currentUri=="/diag");}
    // The real 12-page config uses URL encoding; preserve every field, UTF-8,
    // escaped ampersands/pluses, credentials, and fields beyond multipart's cap.
    for(int pages:{1,3,12}){
        now=0;std::string body="ssid=Home%2BWiFi&token=abc%26def&name=%E7%85%A7%E6%98%8E&pagecount="+std::to_string(pages);
        for(int i=0;i<pages*8;++i)for(char key:{'e','n','x','y'})body+="&"+std::string(1,key)+std::to_string(i)+"=entity"+std::to_string(i);
        WebServer s;WiFiClient c(request("/save","application/x-www-form-urlencoded",body));c.gap=1;
        assert(s._parseRequest(c)&&s.arg("ssid")=="Home+WiFi"&&s.arg("token")=="abc&def"&&s.arg("name")=="照明");
        assert(s.arg("e"+std::to_string(pages*8-1))=="entity"+std::to_string(pages*8-1));
    }
    // Binary OTA: buffer boundaries, zero bytes, and almost-matching boundary strings.
    {now=0;std::string bytes(2300000,'x');for(size_t i=0;i<bytes.size();i+=257)bytes[i]=char(i%256);
     bytes.replace(200,18,"\r\n--m5boundaries!");
     WebServer s;WiFiClient c(request("/update","multipart/form-data; boundary=m5boundary",multipart(bytes)));c.gap=1;
     assert(s._parseRequest(c)&&s.handler.starts==1&&s.handler.ends==1&&s.handler.aborts==0);
     assert(std::string(s.handler.bytes.begin(),s.handler.bytes.end())==bytes);}
    // Stop before a file finishes but leave TCP open: abort once, within 1.5 s.
    {now=0;WebServer s;WiFiClient c(request("/update","multipart/form-data; boundary=m5boundary",multipart("partial",false),10000));
     assert(!s._parseRequest(c)&&!c.open&&s.handler.starts==1&&s.handler.aborts==1&&s.handler.ends==0&&now==1500);}
    // A byte per second evades an idle timeout, but cannot evade the total budget.
    {now=0;WebServer s;auto body=multipart(std::string(200,'x'),false);
     WiFiClient c(request("/update","multipart/form-data; boundary=m5boundary",body,10000));
     c.trickleAfter=c.input.find("\r\n\r\n",c.input.find("Content-Disposition"))+4;c.gap=1000;
     assert(!s._parseRequest(c)&&!c.open&&s.handler.aborts==1&&now==120000);}
    {now=0;WebServer s;WiFiClient c("GET /diag HTTP/1.1\r\nHost: ");assert(!s._parseRequest(c)&&!c.open&&now==1500);}
    {now=0;WebServer s;WiFiClient c("GET /diag HTTP/1.1\r\n\r\n");c.chunk=1;c.gap=1000;assert(!s._parseRequest(c)&&!c.open&&now==5000);}
    {now=0;WebServer s;WiFiClient c("GET /"+std::string(9000,'x'));assert(!s._parseRequest(c)&&!c.open);}
    {now=0;WebServer s;WiFiClient c(request("/display","application/x-www-form-urlencoded","mode=off"));assert(s._parseRequest(c)&&s.arg("mode")=="off");}
    {now=0;WebServer s;WiFiClient c(request("/save","application/x-www-form-urlencoded","a=",100));assert(!s._parseRequest(c)&&!c.open&&now==5000);}
    {now=0;WebServer s;WiFiClient c(request("/save","application/x-www-form-urlencoded","",200000));assert(!s._parseRequest(c)&&!c.open);}
    puts("PASS: real guarded parser, 1/3/12-page forms, UTF-8, 2.3 MB binary OTA, stalls, trickles, deadlines, truncation and abort cleanup");
}
'''
signatures=['static char* readBytesWithTimeout(', 'String WebServer::_readStringUntil(',
            'bool WebServer::_parseRequest(', 'bool WebServer::_collectHeader(',
            'void WebServer::_parseArguments(', 'void WebServer::_uploadWriteByte(',
            'int WebServer::_uploadReadByte(', 'bool WebServer::_parseForm(',
            'String WebServer::urlDecode(', 'bool WebServer::_parseFormUploadAborted(']
with tempfile.TemporaryDirectory(prefix='m5-web-deadlines-') as directory:
    tmp=Path(directory);(tmp/'test.cpp').write_text(prefix+''.join(function(s) for s in signatures)+tests)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-sign-compare',str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True,timeout=10)

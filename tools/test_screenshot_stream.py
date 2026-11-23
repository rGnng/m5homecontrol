#!/usr/bin/env python3
"""Exercise the real BMP handler, including every early-return cleanup path."""
from pathlib import Path
import subprocess
import tempfile
from test_network_io import ROOT, write_headers

source=(ROOT/'src/app_config.cpp').read_text()
a=source.index('static void handleScreenshot()');b=source.index('\nstatic String stationHostname();',a)
PREFIX=r'''
#include "connected_power.h"
#include <cassert>
#include <cstdio>
#include <vector>
static int guards=0,restores=0,reads=0;
static bool sleeping=true,failHeader=false,failRow=false;
static unsigned rowDelay=0;
static std::vector<uint8_t> output;
void beginConnectedPowerWork(){++guards;}void endConnectedPowerWork(){--guards;}
static bool beginDisplayRead(){bool was=sleeping;sleeping=false;return was;}
static void endDisplayRead(bool sleep){++restores;sleeping=sleep;}
struct Client{void stop(){}};
static size_t writeClientWithin(Client&,const uint8_t* bytes,size_t n){
    if((n==54 && failHeader)||(n==960 && failRow))return 0;
    output.insert(output.end(),bytes,bytes+n);return n;
}
struct Server{
    void sendHeader(const char*,const char*){}void setContentLength(size_t n){assert(n==230454);}
    void send(int code,const char*,const char*){assert(code==200);}Client client(){return {};}
}server;
struct Panel{void readRectRGB(int x,int y,int width,int height,uint8_t* bytes){
    assert(x==0 && width==320 && height==1 && !sleeping);++reads;now+=rowDelay;
    for(int i=0;i<320;++i){bytes[i*3]=uint8_t(y);bytes[i*3+1]=uint8_t(i);bytes[i*3+2]=42;}
}};
struct{Panel Display;}M5;
'''
TEST=r'''
static void reset(){now=0;reads=0;restores=0;guards=0;output.clear();failHeader=failRow=false;rowDelay=0;}
int main(){
    reset();handleScreenshot();assert(output.size()==230454 && reads==240 && guards==0 && restores==1 && sleeping);
    assert(output[0]=='B' && output[1]=='M' && output[54]==42 && output[55]==0 && output[56]==239);
    assert(output[output.size()-3]==42 && output.back()==0);
    reset();sleeping=false;handleScreenshot();assert(!sleeping && guards==0 && restores==1);
    reset();sleeping=true;failHeader=true;handleScreenshot();assert(sleeping && guards==0 && restores==1 && reads==0);
    reset();failRow=true;handleScreenshot();assert(sleeping && guards==0 && restores==1 && reads==1);
    reset();rowDelay=100;handleScreenshot();assert(now==5000 && sleeping && guards==0 && restores==1 && reads==50);
    puts("PASS: real BMP bytes, orientation, RGB conversion, one-row memory, timeout and LCD/power restoration");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-screenshot-') as directory:
    tmp=Path(directory);write_headers(tmp)
    # The handler's transport is tested separately; this test checks panel/guard lifetime.
    (tmp/'test.cpp').write_text(PREFIX+source[a:b]+TEST)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',f'-I{tmp}',f'-I{ROOT/"include"}',str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True,timeout=5)

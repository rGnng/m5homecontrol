#pragma once
#include <cstddef>
#include <cstdint>
#include "ui_font.h"
namespace ui_text {
struct Character { uint32_t codepoint; size_t bytes; bool valid; };
inline Character decode(const char* text, size_t length, size_t offset) {
    if (offset >= length) return {0,0,false};
    const auto* b = reinterpret_cast<const uint8_t*>(text);
    uint8_t first = b[offset];
    if (first < 0x80) return {first,1,true};
    unsigned n = first >= 0xc2 && first <= 0xdf ? 2
                 : (first >= 0xe0 && first <= 0xef ? 3
                 : (first >= 0xf0 && first <= 0xf4 ? 4 : 0));
    if (!n || offset+n > length) return {'?',1,false};
    uint32_t cp = first & ((1u << (7-n))-1);
    for (unsigned i=1;i<n;++i) {
        if ((b[offset+i]&0xc0)!=0x80) return {'?',1,false};
        cp=(cp<<6)|(b[offset+i]&0x3f);
    }
    if ((n==2 && cp<0x80)||(n==3 && cp<0x800)||(n==4 && cp<0x10000)||
        cp>0x10ffff||(cp>=0xd800 && cp<=0xdfff)) return {'?',1,false};
    return {cp,n,true};
}
inline size_t previousBoundary(const char* text,size_t length) {
    if (!length) return 0;
    size_t pos=length-1;
    while(pos && (static_cast<uint8_t>(text[pos])&0xc0)==0x80) --pos;
    return pos;
}
template<class Text> Text clean(const Text& input) {
    Text out;out.reserve(input.length());
    for(size_t pos=0;pos<input.length();) {
        Character ch=decode(input.c_str(),input.length(),pos);
        if(!ch.valid) out+='?';
        else if(ch.codepoint>=0x20 && !(ch.codepoint>=0x7f && ch.codepoint<=0x9f))
            for(size_t i=0;i<ch.bytes;++i) out+=input[pos+i];
        pos+=ch.bytes;
    }
    return out;
}
template<class Text> Text forFont(const Text& input) {
    Text safe=clean(input),out;out.reserve(safe.length());
    for(size_t pos=0;pos<safe.length();) {
        Character ch=decode(safe.c_str(),safe.length(),pos);
        if(uiFontContains(ch.codepoint))
            for(size_t i=0;i<ch.bytes;++i) out+=safe[pos+i];
        else out+="\xe2\x96\xa1";
        pos+=ch.bytes;
    }
    return out;
}
template<class Text> Text shorten(const Text& input,size_t limit) {
    if(!limit) return Text{};
    Text safe=clean(input);size_t pos=0,count=0,prefix=0;
    while(pos<safe.length()) {
        Character ch=decode(safe.c_str(),safe.length(),pos);
        if(count<limit-1) prefix=pos+ch.bytes;
        pos+=ch.bytes;++count;
    }
    if(count<=limit) return safe;
    Text out;for(size_t i=0;i<prefix;++i) out+=safe[i];out+='~';return out;
}
}

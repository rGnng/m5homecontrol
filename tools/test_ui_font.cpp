#include "ui_font.h"
#include "ui_font_data.h"
#include "utf8_text.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>

static bool pixel(size_t glyph, size_t x, size_t y) {
    const size_t offset = glyph * UI_FONT_GLYPH_BYTES + y * UI_FONT_ROW_BYTES + x / 8;
    return uiFontBitmap[offset] & (0x80 >> (x % 8));
}
static void reference(uint16_t cp, const uint16_t (&rows)[11]) {
    auto found = std::lower_bound(uiFontCodepoints, uiFontCodepoints + UI_FONT_GLYPH_COUNT, cp);
    assert(*found == cp);
    size_t index = found - uiFontCodepoints;
    for (size_t y=0; y<11; ++y)
        for (size_t x=0; x<10; ++x)
            assert(pixel(index, x*2, y*2) == bool(rows[y] & (1 << (9-x))));
}
int main() {
    static_assert(UI_FONT_WIDTH == 20 && UI_FONT_HEIGHT == 22);
    static_assert((27-UI_FONT_HEIGHT)/2 + UI_FONT_HEIGHT < 27);
    static_assert((24-UI_FONT_HEIGHT)/2 + UI_FONT_HEIGHT <= 24);
    for (size_t i=0; i<UI_FONT_GLYPH_COUNT; ++i) {
        if (i) assert(uiFontCodepoints[i-1] < uiFontCodepoints[i]);
        assert(uiFontAdvances[i] <= UI_FONT_WIDTH);
        for (size_t y=0; y<UI_FONT_HEIGHT; y+=2)
            for (size_t x=0; x<UI_FONT_WIDTH; x+=2) {
                bool dot = pixel(i,x,y);
                assert(pixel(i,x+1,y) == dot && pixel(i,x,y+1) == dot && pixel(i,x+1,y+1) == dot);
                if (dot) assert(x < uiFontAdvances[i]);
            }
    }
    reference('0', {0,0,448,544,544,544,544,544,448,0,0});
    reference(u'あ', {0,64,1020,64,248,340,594,546,452,24,0});
    for (uint16_t cp=32; cp<127; ++cp) assert(uiFontContains(cp));
    for (char16_t cp : u"げごづばぽゔヴ日本語□") if (cp) assert(uiFontContains(cp));
    assert(uiFontAdvance('A') == 12 && uiFontAdvance('0') == 12 && uiFontAdvance(u'あ') == 20);
    for (uint16_t cp='1'; cp<='8'; ++cp) {
        const auto ink = uiFontInkBounds(cp);
        const int x = (24-ink.width)/2-ink.x;
        const int y = (27-1-ink.height)/2-ink.y;
        const int left = x+ink.x, right = 24-(x+ink.x+ink.width);
        const int top = y+ink.y, bottom = 26-(y+ink.y+ink.height);
        assert(left == right && top == bottom);
        assert(left == (cp == '1' ? 9 : 7) && top == 6);
    }
    for (uint16_t cp : {u'A', u'あ', u'げ', u'g'}) {
        const auto ink = uiFontInkBounds(cp);
        const int top = (27-22)/2 + ink.y;
        const int bottom = 26-(top+ink.height);
        assert(top >= 0 && bottom >= 0);
        const int titleTop = (24-22)/2 + ink.y;
        assert(titleTop >= 0 && titleTop+ink.height <= 24);
    }
    assert(!uiFontContains(0x1f600) && !uiFontContains(0xffff));
    assert(ui_text::clean(std::string("日本語\nABC")) == "日本語ABC");
    assert(ui_text::clean(std::string("\xc0\xaf")) == "??");
    assert(ui_text::forFont(std::string("日本😀")) == "日本□");
    assert(ui_text::shorten(std::string("日本語ABC"),4) == "日本語~");
    assert(ui_text::shorten(std::string("日本語"),3) == "日本語");
    assert(ui_text::previousBoundary("Aあ",4) == 1);
    std::cout << "PASS: native reference pixels, exact 2x dots for every glyph, layout bounds, UTF-8 and coverage\n";
}

#include <M5GFX.h>
#include "ui_font.h"
#include "ui_font_data.h"
namespace {
class NativeDotFont final : public lgfx::BDFfont {
public:
    NativeDotFont()
        : BDFfont(uiFontBitmap, uiFontCodepoints, UI_FONT_GLYPH_COUNT,
                  UI_FONT_WIDTH, UI_FONT_WIDTH, UI_FONT_HEIGHT, UI_FONT_BASELINE, UI_FONT_HEIGHT) {}
    bool updateFontMetric(lgfx::FontMetrics* metrics, uint16_t cp) const override {
        metrics->width = metrics->x_advance = uiFontAdvance(cp);
        return true;
    }
    size_t drawChar(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint16_t cp,
                    const lgfx::TextStyle* style, lgfx::FontMetrics* metrics,
                    int32_t& filledX) const override {
        BDFfont::drawChar(gfx, x, y, cp, style, metrics, filledX);
        return (static_cast<int32_t>(65536 * style->size_x) * uiFontAdvance(cp)) >> 16;
    }
};
}
const lgfx::IFont* uiFont() {
    static const NativeDotFont font;
    return &font;
}

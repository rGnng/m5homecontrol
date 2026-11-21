#pragma once
#include <cstdint>
namespace lgfx { inline namespace v1 { struct IFont; } }
const lgfx::IFont* uiFont();
bool uiFontContains(uint32_t codepoint);
uint8_t uiFontAdvance(uint32_t codepoint);
struct UiFontInkBounds { int x, y, width, height; };
UiFontInkBounds uiFontInkBounds(uint32_t codepoint);

#pragma once

#include <cstdint>

// Translate the Calculator3's physical matrix into the user's key layout.
// Serial input already uses logical keys and must not pass through this map.
inline char remapKeypadKey(uint8_t raw) {
    switch (raw) {
        case '7': return '1';
        case '8': return '2';
        case '9': return '3';
        case '1': return '7';
        case '2': return '8';
        case '3': return '9';
        case '-': return '+'; // r3c4
        case '+': return '-'; // r4c4
        case '0': case '4': case '5': case '6':
        case 'A': case '\b': case 'M': case '%': case '=': case '\r':
            return static_cast<char>(raw);
        default: return 0;
    }
}

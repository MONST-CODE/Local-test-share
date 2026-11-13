#pragma once
#include <cstdint>
#include <string_view>
namespace plpt {
// UTF-8 canonique ; pas d'ESC/NUL/contrôles terminal, excepté LF et TAB.
inline bool valid_text(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i++]);
        if (b < 0x80) { if ((b < 0x20 && b != '\n' && b != '\t') || b == 0x7F) return false; continue; }
        unsigned count;
        std::uint32_t cp, minimum;
        if (b >= 0xC2 && b <= 0xDF) { count = 1; cp = b & 0x1F; minimum = 0x80; }
        else if (b >= 0xE0 && b <= 0xEF) { count = 2; cp = b & 0x0F; minimum = 0x800; }
        else if (b >= 0xF0 && b <= 0xF4) { count = 3; cp = b & 0x07; minimum = 0x10000; }
        else return false;
        if (s.size() - i < count) return false;
        for (unsigned k = 0; k < count; ++k) {
            const auto next = static_cast<unsigned char>(s[i++]);
            if ((next & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3F);
        }
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0x80 && cp <= 0x9F)) return false;
    }
    return true;
}
} // namespace plpt

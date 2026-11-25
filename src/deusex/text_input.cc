#include "deusex/text_input.h"

#include <array>
#include <cstdint>

namespace dx::textinput {
    namespace {
        // The characters Windows-1252 puts at 0x80-0x9F, or 0 where it has none.
        constexpr std::array<uint16_t, 32> windows1252 {
            0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
            0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178
        };

        // The game's byte for a code point, or -1.
        int toByte(uint32_t code)
        {
            if (code < 0x80 || (code >= 0xa0 && code <= 0xff)) {
                return static_cast<int>(code);
            }
            for (size_t i = 0; i < windows1252.size(); i++) {
                if (windows1252[i] != 0 && windows1252[i] == code) {
                    return static_cast<int>(0x80 + i);
                }
            }
            return -1;
        }
    }

    std::string fromUtf8(std::string_view text)
    {
        std::string result;
        size_t i = 0;
        while (i < text.size()) {
            uint8_t lead = static_cast<uint8_t>(text[i]);
            size_t extra = 0;
            uint32_t code = lead;
            uint32_t minimum = 0;
            if (lead >= 0xc2 && lead <= 0xdf) {
                extra = 1;
                code = lead & 0x1fu;
                minimum = 0x80;
            } else if (lead >= 0xe0 && lead <= 0xef) {
                extra = 2;
                code = lead & 0x0fu;
                minimum = 0x800;
            } else if (lead >= 0xf0 && lead <= 0xf4) {
                extra = 3;
                code = lead & 0x07u;
                minimum = 0x10000;
            } else if (lead >= 0x80) {
                // A stray continuation byte, or a lead that is never valid.
                i++;
                continue;
            }
            size_t length = 1;
            while (length <= extra && i + length < text.size() && (static_cast<uint8_t>(text[i + length]) & 0xc0) == 0x80) {
                code = (code << 6) | (static_cast<uint8_t>(text[i + length]) & 0x3fu);
                length++;
            }
            i += length;
            if (length <= extra || code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
                continue;
            }
            if (int byte = toByte(code); byte >= 0) {
                result.push_back(static_cast<char>(byte));
            }
        }
        return result;
    }

    std::string toUtf8(std::string_view text)
    {
        std::string result;
        for (char c : text) {
            uint32_t code = static_cast<uint8_t>(c);
            if (code >= 0x80 && code < 0xa0) {
                code = windows1252[code - 0x80];
                if (code == 0) {
                    continue;
                }
            }
            if (code < 0x80) {
                result.push_back(static_cast<char>(code));
            } else if (code < 0x800) {
                result.push_back(static_cast<char>(0xc0 | (code >> 6)));
                result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            } else {
                result.push_back(static_cast<char>(0xe0 | (code >> 12)));
                result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            }
        }
        return result;
    }

    std::string consolePaste(std::string_view text)
    {
        std::string result;
        char previous = 0;
        for (char c : fromUtf8(text)) {
            uint8_t code = static_cast<uint8_t>(c);
            if (c == '\n' && previous == '\r') {
                // A CR LF pair is one line break.
            } else if (c == '\n' || c == '\r' || c == '\t') {
                result.push_back(' ');
            } else if (code >= 0x20 && code != 0x7f && c != '~' && c != '`') {
                result.push_back(c);
            }
            previous = c;
        }
        while (!result.empty() && result.back() == ' ') {
            result.pop_back();
        }
        return result;
    }
}

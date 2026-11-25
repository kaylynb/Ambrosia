#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dx::compiler {
    // Case-insensitive search like the original's appStrfind: a match only counts where the character
    // before it is not an ASCII letter or digit, so "Name=" is not found inside "FontName=".
    size_t findKey(std::string_view stream, std::string_view match);
    bool parseCommand(std::string_view& stream, std::string_view match);
    bool parseString(std::string_view stream, std::string_view match, std::string& value);
    bool parseInt(std::string_view stream, std::string_view match, int32_t& value);
    bool parseByte(std::string_view stream, std::string_view match, uint8_t& value);
    bool parseFloat(std::string_view stream, std::string_view match, float& value);
    bool parseBool(std::string_view stream, std::string_view match, bool& value);
}

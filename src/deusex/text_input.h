#pragma once

#include <string>
#include <string_view>

namespace dx::textinput {
    // Typed UTF-8 text as the game's 8-bit Windows-1252 characters, one byte per character.
    // Characters it has no code for, and malformed bytes, are dropped.
    std::string fromUtf8(std::string_view text);
    // The game's characters as UTF-8, for a system keyboard to show.
    std::string toUtf8(std::string_view text);
    // Clipboard text (UTF-8) as one line for the console to type: line breaks and tabs become
    // spaces, other control characters and the console's own toggle keys (~ and `) are dropped,
    // and spaces at the end are trimmed.
    std::string consolePaste(std::string_view text);
}

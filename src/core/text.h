#pragma once

#include <string>
#include <string_view>

namespace dx {
    // A copy of the text with each character passed through std::tolower.
    std::string lowered(std::string_view text);
    // Whether two texts match ignoring case, comparing each pair of characters through
    // std::tolower.
    bool iequals(std::string_view a, std::string_view b);
}

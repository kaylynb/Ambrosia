#pragma once

#include <compare>
#include <span>
#include <string>
#include <vector>

namespace dx::resolutions {
    // The sizes offered in the display options. Fullscreen is borderless at the desktop's
    // resolution, so it offers only that; windowed offers standard sizes that fit on the
    // display, in its aspect ratio and the common 16:9 and 16:10. Exclusive fullscreen, an ini setting, offers the display's
    // own modes in that aspect ratio instead.

    struct Size {
        int width = 0;
        int height = 0;
        auto operator<=>(const Size&) const = default;
    };

    // Nothing shorter is offered, though SetRes and the ini still accept any size.
    inline constexpr int minimumHeight = 720;
    // MenuChoice_Resolution keeps this many.
    inline constexpr size_t menuLimit = 16;

    // Standard window sizes no shorter than minimumHeight that fit within room, in native's
    // aspect ratio and in 16:9 and 16:10, narrowest first, with current always among them.
    std::vector<Size> windowed(Size native, Size room, Size current);
    // The display modes in native's aspect ratio no shorter than minimumHeight, narrowest first,
    // with native and current always among them.
    std::vector<Size> exclusive(std::span<const Size> modes, Size native, Size current);
    // The window size to use when none is set: the largest standard size of native's shape
    // that fits in room, else the largest windowed size, else room itself.
    Size defaultWindow(Size native, Size room);
    // "1280x720 1920x1080", as the GetRes console command answers; a single size ends with a
    // space, for MenuChoice_Resolution.
    std::string format(const std::vector<Size>& sizes);
}

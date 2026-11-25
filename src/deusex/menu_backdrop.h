#pragma once

#include "deusex/menu_layout.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace dx::deusex {
    // A generated menu backdrop in the style of the original menus' art: BGRA, rows top first,
    // grey (the menu tints it) and see-through (alpha 0) where the widgets sit and along its
    // traces.
    struct Backdrop {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> bgra;
    };

    // The backdrop for a layout: its slots cut out, traces routed between them that never cross
    // and always end on something, the empty areas marked off by single lines, the originals'
    // connector and double bars, and shading fitted to the originals. The same layout and seed
    // give the same pixels on every platform: the routing is whole numbers, its random numbers
    // its own (no std:: distribution, which differ between standard libraries), and the shading
    // only adds, multiplies and divides, in this file compiled without fused multiply-adds.
    Backdrop generateBackdrop(const MenuLayout& layout, uint64_t seed);

    // The seed a screen's backdrop is made with: picked by hand for some screens, else from the
    // screen's name, so it stays the same from one build to the next.
    uint64_t backdropSeed(std::string_view screen);
}

#include "engine/resolutions.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <vector>

using dx::resolutions::Size;

TEST_CASE("windowed sizes are standard sizes that fit on the display, in its shape and in 16:9 and 16:10", "[resolutions]")
{
    // A 1440p monitor less its taskbar and the window's frame.
    std::vector<Size> sizes = dx::resolutions::windowed({ 2560, 1440 }, { 2544, 1360 }, { 1920, 1080 });
    CHECK(dx::resolutions::format(sizes) == "1280x720 1280x800 1366x768 1440x900 1600x900 1680x1050 1920x1080 1920x1200");

    // A 16:10 display.
    sizes = dx::resolutions::windowed({ 1920, 1200 }, { 1904, 1130 }, { 1440, 900 });
    CHECK(dx::resolutions::format(sizes) == "1280x720 1280x800 1366x768 1440x900 1600x900 1680x1050");

    // An ultrawide display offers the usual sizes too, and the ultrawide ones that fit; those
    // differ slightly in shape but all count as 21:9.
    sizes = dx::resolutions::windowed({ 3440, 1440 }, { 3424, 1360 }, { 2560, 1080 });
    CHECK(dx::resolutions::format(sizes) == "1280x720 1280x800 1366x768 1440x900 1600x900 1680x1050 1920x1080 1920x1200 2560x1080");
}

TEST_CASE("the current size is always offered, even an odd or small one", "[resolutions]")
{
    std::vector<Size> sizes = dx::resolutions::windowed({ 1920, 1080 }, { 1904, 1001 }, { 1001, 613 });
    CHECK(dx::resolutions::format(sizes) == "1001x613 1280x720 1280x800 1366x768 1440x900 1600x900");
    // A display too small for any standard size still offers the window's own.
    sizes = dx::resolutions::windowed({ 1366, 768 }, { 1350, 690 }, { 1200, 680 });
    CHECK(dx::resolutions::format(sizes) == "1200x680 ");
}

TEST_CASE("an unusual aspect ratio gets sizes of its own shape", "[resolutions]")
{
    // 2:1, between 16:9 and 21:9.
    std::vector<Size> sizes = dx::resolutions::windowed({ 2000, 1000 }, { 1984, 920 }, {});
    CHECK(std::find(sizes.begin(), sizes.end(), Size { 1440, 720 }) != sizes.end());
    CHECK(std::find(sizes.begin(), sizes.end(), Size { 1800, 900 }) != sizes.end());
    for (Size size : sizes) {
        CHECK(size.height >= dx::resolutions::minimumHeight);
        CHECK(size.width % 2 == 0);
    }
}

TEST_CASE("exclusive fullscreen offers the display's modes in its shape and no low ones", "[resolutions]")
{
    std::vector<Size> modes { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 1024 }, { 1600, 900 }, { 1920, 1080 }, { 1920, 1080 }, { 1176, 664 } };
    std::vector<Size> sizes = dx::resolutions::exclusive(modes, { 1920, 1080 }, { 1920, 1080 });
    CHECK(dx::resolutions::format(sizes) == "1280x720 1600x900 1920x1080");
}

TEST_CASE("no more sizes are offered than the menu holds, keeping the current one", "[resolutions]")
{
    std::vector<Size> modes;
    for (int height = 720; height < 720 + 40 * 9; height += 9) {
        modes.push_back({ height * 16 / 9, height });
    }
    Size current = modes.front();
    std::vector<Size> sizes = dx::resolutions::exclusive(modes, { 3840, 2160 }, current);
    CHECK(sizes.size() == dx::resolutions::menuLimit);
    CHECK(sizes.front() == current);
    CHECK(sizes.back() == Size { 3840, 2160 });
}

TEST_CASE("the default window is the largest standard size that fits", "[resolutions]")
{
    CHECK(dx::resolutions::defaultWindow({ 1920, 1080 }, { 1904, 1001 }) == Size { 1600, 900 });
    CHECK(dx::resolutions::defaultWindow({ 3840, 2160 }, { 3824, 2080 }) == Size { 3200, 1800 });
    // Of the display's own shape where one fits, even with taller 16:10 sizes on offer.
    CHECK(dx::resolutions::defaultWindow({ 3440, 1440 }, { 3424, 1360 }) == Size { 2560, 1080 });
    // Nothing fits on a small laptop screen, so the window fills its usable area.
    CHECK(dx::resolutions::defaultWindow({ 1366, 768 }, { 1350, 690 }) == Size { 1350, 690 });
}

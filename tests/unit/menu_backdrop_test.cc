#include "deusex/menu_backdrop.h"

#include <catch_amalgamated.hpp>

#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <vector>

namespace {
    using Kind = dx::deusex::MenuSlot::Kind;

    // The Sound screen as the engine lays it out with Ambrosia's changes: four sliders, the
    // Audio Mixer and the help box.
    dx::deusex::MenuLayout soundLayout()
    {
        dx::deusex::MenuLayout layout;
        layout.screen = "MenuScreenSound";
        layout.width = 537;
        layout.height = 264;
        for (int row = 0; row < 4; row++) {
            int y = 26 + 36 * row;
            layout.slots.push_back({ Kind::Button, 6, y, 245, 21 });
            layout.slots.push_back({ Kind::Track, 276, y - 1, 179, 23 });
            layout.slots.push_back({ Kind::Field, 460, y, 62, 21 });
        }
        layout.slots.push_back({ Kind::Button, 6, 170, 245, 21 });
        layout.slots.push_back({ Kind::Field, 276, 170, 85, 21 });
        layout.slots.push_back({ Kind::TextBox, 6, 209, 516, 29 });
        return layout;
    }

    bool cut(const dx::deusex::Backdrop& art, int x, int y)
    {
        return art.bgra[(static_cast<size_t>(y) * static_cast<size_t>(art.width) + static_cast<size_t>(x)) * 4 + 3] == 0;
    }

    // Pads a line ends in and goes no further: a 3px square with clear corners round it that has
    // fewer than two lines out of it (one in and one on, or the two of an L).
    int deadPads(const dx::deusex::Backdrop& art)
    {
        int dead = 0;
        for (int y = 3; y < art.height - 3; y++) {
            for (int x = 3; x < art.width - 3; x++) {
                bool square = true;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        square = square && cut(art, x + dx, y + dy);
                    }
                }
                if (!square || cut(art, x - 2, y - 2) || cut(art, x + 2, y - 2) || cut(art, x - 2, y + 2) || cut(art, x + 2, y + 2)) {
                    continue;
                }
                int ways = 0;
                for (int d = -1; d <= 1; d++) {
                    ways += (cut(art, x + d, y - 2) ? 1 : 0) + (cut(art, x + d, y + 2) ? 1 : 0) + (cut(art, x - 2, y + d) ? 1 : 0) + (cut(art, x + 2, y + d) ? 1 : 0);
                }
                dead += ways < 2 ? 1 : 0;
            }
        }
        return dead;
    }

    uint64_t fnv(const std::vector<uint8_t>& bytes)
    {
        uint64_t hash = 14695981039346656037ull;
        for (uint8_t b : bytes) {
            hash ^= b;
            hash *= 1099511628211ull;
        }
        return hash;
    }
}

TEST_CASE("a menu backdrop is the same for the same layout and seed", "[menu]")
{
    dx::deusex::MenuLayout layout = soundLayout();
    dx::deusex::Backdrop a = dx::deusex::generateBackdrop(layout, 7);
    dx::deusex::Backdrop b = dx::deusex::generateBackdrop(layout, 7);
    dx::deusex::Backdrop other = dx::deusex::generateBackdrop(layout, 8);
    REQUIRE(a.width == 537);
    REQUIRE(a.height == 264);
    REQUIRE(a.bgra.size() == static_cast<size_t>(537 * 264 * 4));
    CHECK(a.bgra == b.bgra);
    CHECK(a.bgra != other.bgra);
    // A screen's seed is its own, and the same each time.
    CHECK(dx::deusex::backdropSeed("MenuScreenSound") == dx::deusex::backdropSeed("MenuScreenSound"));
    CHECK(dx::deusex::backdropSeed("MenuScreenSound") != dx::deusex::backdropSeed("MenuScreenDisplay"));
}

TEST_CASE("a menu backdrop is cut out where the widgets sit", "[menu]")
{
    dx::deusex::MenuLayout layout = soundLayout();
    dx::deusex::Backdrop art = dx::deusex::generateBackdrop(layout, dx::deusex::backdropSeed(layout.screen));
    for (const dx::deusex::MenuSlot& slot : layout.slots) {
        int cx = slot.x + slot.width / 2;
        int cy = slot.y + slot.height / 2;
        if (slot.kind == Kind::Button || slot.kind == Kind::Track) {
            CHECK(cut(art, cx, cy));
        } else {
            // An outline round a grey inside.
            CHECK(cut(art, slot.x + slot.width / 2, slot.y));
            CHECK_FALSE(cut(art, cx, cy));
        }
    }
    // The grey is grey, within the originals' range, and the cut-outs see-through.
    for (size_t i = 0; i < art.bgra.size(); i += 4) {
        if (art.bgra[i + 3] == 0) {
            CHECK(art.bgra[i] == 0);
            continue;
        }
        REQUIRE(art.bgra[i] == art.bgra[i + 1]);
        REQUIRE(art.bgra[i] == art.bgra[i + 2]);
        REQUIRE(art.bgra[i] >= 16);
        REQUIRE(art.bgra[i] <= 165);
    }
}

TEST_CASE("a menu backdrop's slots have the originals' corners", "[menu]")
{
    // Measured from MenuSoundBackground: a slider's track is cut 3px at each corner, a value
    // field's outline 2px at the top left and 6px at the bottom right, inside and out.
    dx::deusex::Backdrop art = dx::deusex::generateBackdrop(soundLayout(), 1);
    // The track at (276, 25) to (454, 47).
    CHECK_FALSE(cut(art, 278, 25));
    CHECK(cut(art, 279, 25));
    CHECK(cut(art, 451, 25));
    CHECK_FALSE(cut(art, 452, 25));
    CHECK_FALSE(cut(art, 278, 47));
    CHECK(cut(art, 279, 47));
    CHECK(cut(art, 451, 47));
    CHECK_FALSE(cut(art, 452, 47));
    // The field at (460, 26) to (521, 46): its outline, and the inside's corners.
    CHECK_FALSE(cut(art, 461, 26));
    CHECK(cut(art, 462, 26));
    CHECK(cut(art, 460, 28));
    CHECK(cut(art, 515, 46));
    CHECK_FALSE(cut(art, 516, 46));
    CHECK(cut(art, 464, 29));
    CHECK_FALSE(cut(art, 465, 29));
    CHECK(cut(art, 513, 43));
    CHECK_FALSE(cut(art, 512, 43));
}

TEST_CASE("a menu backdrop's lines end on something", "[menu]")
{
    dx::deusex::MenuLayout layout = soundLayout();
    for (uint64_t seed = 1; seed <= 6; seed++) {
        dx::deusex::Backdrop art = dx::deusex::generateBackdrop(layout, seed);
        int w = art.width;
        int h = art.height;
        // A line end: a cut-out with one cut-out neighbour, off the window's edge. The only ones
        // are the connector's two lines, which stop short of its dots as the originals' do, above
        // the top row of slots.
        int ends = 0;
        for (int y = 1; y < h - 1; y++) {
            for (int x = 1; x < w - 1; x++) {
                if (!cut(art, x, y)) {
                    continue;
                }
                int n = 0;
                int nx[2] = {};
                int ny[2] = {};
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if ((dx || dy) && cut(art, x + dx, y + dy)) {
                            if (n < 2) {
                                nx[n] = x + dx;
                                ny[n] = y + dy;
                            }
                            n++;
                        }
                    }
                }
                // One neighbour, or two side by side: the end of a line, or a pixel stub on one.
                bool spur = n == 2 && std::abs(nx[0] - nx[1]) + std::abs(ny[0] - ny[1]) == 1;
                if (n == 1 || spur) {
                    ends++;
                    CHECK(y < 25);
                }
            }
        }
        CHECK(ends <= 2);
        CHECK(deadPads(art) == 0);

        // Every piece is joined to a slot or the window's edge, but for the connector's dots and
        // bars, above the top row.
        std::vector<uint8_t> held(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
        std::deque<std::pair<int, int>> queue;
        auto hold = [&](int x, int y) {
            size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            if (cut(art, x, y) && !held[i]) {
                held[i] = 1;
                queue.push_back({ x, y });
            }
        };
        for (int x = 0; x < w; x++) {
            hold(x, 0);
            hold(x, h - 1);
        }
        for (int y = 0; y < h; y++) {
            hold(0, y);
            hold(w - 1, y);
        }
        for (const dx::deusex::MenuSlot& slot : layout.slots) {
            hold(slot.x + slot.width / 2, slot.y);
        }
        while (!queue.empty()) {
            auto [x, y] = queue.front();
            queue.pop_front();
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (x + dx >= 0 && y + dy >= 0 && x + dx < w && y + dy < h) {
                        hold(x + dx, y + dy);
                    }
                }
            }
        }
        int floating = 0;
        for (int y = 25; y < h; y++) {
            for (int x = 0; x < w; x++) {
                floating += cut(art, x, y) && !held[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] ? 1 : 0;
            }
        }
        CHECK(floating == 0);
    }
}

TEST_CASE("a menu backdrop keeps off what the screen draws itself", "[menu]")
{
    // As the pad layout's: recessed boxes down both sides, and a big area in the middle the
    // screen draws over, at the largest size it opens.
    dx::deusex::MenuLayout layout;
    layout.screen = "MenuScreenControllerLayout";
    layout.width = 1000;
    layout.height = 580;
    for (int row = 0; row < 8; row++) {
        layout.slots.push_back({ Kind::TextBox, 40, 60 + 60 * row, 200, 40 });
        layout.slots.push_back({ Kind::TextBox, 760, 60 + 60 * row, 200, 40 });
    }
    layout.drawn.assign(static_cast<size_t>(layout.width * layout.height), 0);
    for (int y = 80; y < 500; y++) {
        for (int x = 300; x < 700; x++) {
            layout.drawn[static_cast<size_t>(y * layout.width + x)] = 1;
        }
    }
    for (uint64_t seed = 1; seed <= 12; seed++) {
        CHECK(deadPads(dx::deusex::generateBackdrop(layout, seed)) == 0);
    }
    dx::deusex::Backdrop art = dx::deusex::generateBackdrop(layout, 3);
    int cuts = 0;
    for (int y = 80; y < 500; y++) {
        for (int x = 300; x < 700; x++) {
            cuts += cut(art, x, y) ? 1 : 0;
        }
    }
    CHECK(cuts == 0);
    // The boxes are cut out as usual, and traces reach them.
    CHECK(cut(art, 140, 60));
    CHECK_FALSE(cut(art, 140, 80));
}

TEST_CASE("a menu backdrop's pixels are the same on every platform", "[menu]")
{
    // The routing is whole numbers and the shading built without fused multiply-adds, so this
    // hash is the same from MSVC, GCC and Clang. A change to how backdrops are made changes it:
    // look at the new art (--dump-menu-layouts) and set it again.
    dx::deusex::Backdrop art = dx::deusex::generateBackdrop(soundLayout(), 1);
    CHECK(fnv(art.bgra) == 0x034cff0785d35296ull);
}

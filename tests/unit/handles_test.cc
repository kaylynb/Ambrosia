#include "core/scoped_value.h"
#include "core/sdl_handles.h"
#include "fixtures.h"
#include "render/gl_handle.h"

#include <SDL3/SDL.h>

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {
    // The names the test handle below has deleted, in order.
    std::vector<uint32_t>& deleted()
    {
        static std::vector<uint32_t> names;
        return names;
    }

    void recordDelete(uint32_t id)
    {
        deleted().push_back(id);
    }

    using TestHandle = dx::GLHandle<recordDelete>;
}

TEST_CASE("a GL handle deletes its name once, when it is reset, reassigned or destroyed", "[handles]")
{
    deleted().clear();
    {
        TestHandle empty;
        CHECK_FALSE(empty);
        CHECK(empty.get() == 0);
    }
    CHECK(deleted().empty());

    {
        TestHandle handle(7);
        CHECK(handle);
        CHECK(handle.get() == 7);
        handle.reset(8);
        CHECK(deleted() == std::vector<uint32_t> { 7 });
        // Taking the name it holds already deletes nothing.
        handle.reset(8);
        CHECK(deleted() == std::vector<uint32_t> { 7 });
    }
    CHECK(deleted() == std::vector<uint32_t> { 7, 8 });
}

TEST_CASE("a GL handle moves its name without deleting it, and gives it up with release", "[handles]")
{
    deleted().clear();
    TestHandle first(3);
    TestHandle second(std::move(first));
    CHECK(first.get() == 0);
    CHECK(second.get() == 3);
    CHECK(deleted().empty());

    TestHandle third(4);
    third = std::move(second);
    CHECK(deleted() == std::vector<uint32_t> { 4 });
    CHECK(third.get() == 3);

    // Moving a handle onto itself keeps its name.
    TestHandle& same = third;
    third = std::move(same);
    CHECK(third.get() == 3);
    CHECK(deleted() == std::vector<uint32_t> { 4 });

    CHECK(third.release() == 3);
    CHECK_FALSE(third);
    CHECK(deleted() == std::vector<uint32_t> { 4 });
}

TEST_CASE("a scoped value puts the old one back however its scope ends", "[handles]")
{
    int value = 1;
    {
        dx::ScopedValue<int> scope(value, 2);
        CHECK(value == 2);
    }
    CHECK(value == 1);

    try {
        dx::ScopedValue<int> scope(value, 3);
        throw 0;
    } catch (int) {
    }
    CHECK(value == 1);

    std::string text = "outer";
    {
        dx::ScopedValue<std::string> scope(text, "inner");
        {
            dx::ScopedValue<std::string> nested(text, "innermost");
            CHECK(text == "innermost");
        }
        CHECK(text == "inner");
    }
    CHECK(text == "outer");
}

TEST_CASE("saveBmp writes the pixels top row first, as SDL reads them back", "[handles]")
{
    dx::test::TempDir dir("handles-bmp");
    std::string path = (dir / "two.bmp").string();
    // Two rows of one pixel: red over blue, as RGBA bytes.
    std::vector<uint8_t> rgba = { 255, 0, 0, 255, 0, 0, 255, 255 };
    REQUIRE(dx::sdl::saveBmp(path, rgba, 1, 2));

    dx::sdl::Surface loaded(SDL_LoadBMP(path.c_str()));
    REQUIRE(loaded);
    CHECK(loaded->w == 1);
    CHECK(loaded->h == 2);
    dx::sdl::Surface converted(SDL_ConvertSurface(loaded.get(), SDL_PIXELFORMAT_ABGR8888));
    REQUIRE(converted);
    const auto* pixels = static_cast<const uint8_t*>(converted->pixels);
    const uint8_t* top = pixels;
    const uint8_t* bottom = pixels + converted->pitch;
    CHECK((top[0] == 255 && top[1] == 0 && top[2] == 0));
    CHECK((bottom[0] == 0 && bottom[1] == 0 && bottom[2] == 255));
}

// Under ASan this also checks that nothing is lost: SDL_SaveBMP leaked its converted surface here.
TEST_CASE("saveBmp fails, with nothing written, when the file cannot be made", "[handles]")
{
    dx::test::TempDir dir("handles-bmp-missing");
    std::string path = (dir / "no-such-folder" / "one.bmp").string();
    std::vector<uint8_t> rgba = { 1, 2, 3, 255 };
    CHECK_FALSE(dx::sdl::saveBmp(path, rgba, 1, 1));
}

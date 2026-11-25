#include "game/touch_cursor.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

namespace {
    struct Recorder {
        bool wanted = true;
        std::vector<std::string> events;

        dx::game::TouchCursor cursor()
        {
            return dx::game::TouchCursor({
                .wantsCursor = [this] { return wanted; },
                .move = [this](float x, float y) { events.push_back("move " + std::to_string(static_cast<int>(x)) + "," + std::to_string(static_cast<int>(y))); },
                .button = [this](bool pressed) { events.push_back(pressed ? "press" : "release"); },
                .released = [this] { events.push_back("released"); },
            });
        }
    };

    using Fingers = std::vector<dx::game::Finger>;
}

TEST_CASE("a tap moves the cursor to the finger and clicks there", "[touch]")
{
    Recorder recorder;
    dx::game::TouchCursor touch = recorder.cursor();
    touch.update(Fingers { { 7, 100, 200 } });
    CHECK(touch.touching());
    touch.update(Fingers {});
    CHECK_FALSE(touch.touching());
    CHECK(recorder.events == std::vector<std::string> { "move 100,200", "press", "release", "released" });
}

TEST_CASE("a drag holds the button while the cursor follows the finger", "[touch]")
{
    Recorder recorder;
    dx::game::TouchCursor touch = recorder.cursor();
    touch.update(Fingers { { 1, 10, 10 } });
    touch.update(Fingers { { 1, 10, 10 } });
    touch.update(Fingers { { 1, 40, 12 } });
    touch.update(Fingers { { 1, 90, 15 } });
    touch.update(Fingers {});
    CHECK(recorder.events == std::vector<std::string> { "move 10,10", "press", "move 40,12", "move 90,15", "release", "released" });
}

TEST_CASE("a finger that lands in play does nothing, even once a menu opens under it", "[touch]")
{
    Recorder recorder;
    recorder.wanted = false;
    dx::game::TouchCursor touch = recorder.cursor();
    touch.update(Fingers { { 3, 50, 50 } });
    recorder.wanted = true;
    touch.update(Fingers { { 3, 60, 50 } });
    CHECK(recorder.events.empty());
    touch.update(Fingers {});
    touch.update(Fingers { { 4, 70, 80 } });
    CHECK(recorder.events == std::vector<std::string> { "move 70,80", "press" });
}

TEST_CASE("a second finger is ignored, and a release still lets go if the click closed the menu", "[touch]")
{
    Recorder recorder;
    dx::game::TouchCursor touch = recorder.cursor();
    touch.update(Fingers { { 1, 10, 10 } });
    touch.update(Fingers { { 1, 10, 10 }, { 2, 300, 300 } });
    recorder.wanted = false;
    touch.update(Fingers { { 2, 300, 300 } });
    // The second finger, left on the screen, stays ignored after the first lifts.
    recorder.wanted = true;
    touch.update(Fingers { { 2, 310, 300 } });
    CHECK(recorder.events == std::vector<std::string> { "move 10,10", "press", "release", "released" });
}

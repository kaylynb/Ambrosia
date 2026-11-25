#include "core/log.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {
    // The lines a listener is handed while it lives, as "category|text".
    struct Received {
        std::vector<std::string> lines;
        dx::log::Listener listener { [this](const dx::log::Entry& entry) {
            lines.push_back(std::string(entry.category) + "|" + std::string(entry.text));
        } };
    };

    // Puts the level back for the tests after.
    struct LevelGuard {
        ~LevelGuard()
        {
            dx::log::setLevel(dx::log::Level::Info);
        }
    };
}

TEST_CASE("a logged text is split into lines, with carriage returns and a final newline dropped", "[log]")
{
    Received received;
    dx::log::write(dx::log::Level::Info, "LogTest", "first\r\nsecond\n\nfourth\n");
    CHECK(received.lines == std::vector<std::string> { "LogTest|first", "LogTest|second", "LogTest|", "LogTest|fourth" });
}

TEST_CASE("lines below the level are dropped before any listener sees them", "[log]")
{
    LevelGuard guard;
    Received received;
    dx::log::setLevel(dx::log::Level::Warning);
    CHECK_FALSE(dx::log::enabled(dx::log::Level::Info));
    CHECK(dx::log::enabled(dx::log::Level::Error));
    dx::log::info("LogTest", "quiet");
    dx::log::warn("LogTest", "loud {}", 1);
    dx::log::error("LogTest", "louder");
    CHECK(received.lines == std::vector<std::string> { "LogTest|loud 1", "LogTest|louder" });
}

TEST_CASE("a listener hears only what is written while it lives", "[log]")
{
    Received outer;
    {
        Received inner;
        dx::log::info("LogTest", "both");
        CHECK(inner.lines == std::vector<std::string> { "LogTest|both" });
    }
    dx::log::info("LogTest", "outer only");
    CHECK(outer.lines == std::vector<std::string> { "LogTest|both", "LogTest|outer only" });
}

TEST_CASE("a listener that logs does not wait on its own lock, and its line reaches no listener", "[log]")
{
    std::vector<std::string> heard;
    dx::log::Listener echo([&](const dx::log::Entry& entry) {
        heard.push_back(std::string(entry.text));
        dx::log::info("LogTest", "echo of {}", entry.text);
    });
    Received other;
    dx::log::info("LogTest", "original");
    CHECK(heard == std::vector<std::string> { "original" });
    CHECK(other.lines == std::vector<std::string> { "LogTest|original" });
    // The lock was let go: a later line is handed out as usual.
    dx::log::info("LogTest", "later");
    CHECK(heard == std::vector<std::string> { "original", "later" });
}

TEST_CASE("each level has its letter for the file", "[log]")
{
    CHECK(dx::log::levelMark(dx::log::Level::Debug) == 'D');
    CHECK(dx::log::levelMark(dx::log::Level::Info) == 'I');
    CHECK(dx::log::levelMark(dx::log::Level::Warning) == 'W');
    CHECK(dx::log::levelMark(dx::log::Level::Error) == 'E');
}

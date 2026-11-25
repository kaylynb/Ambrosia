#include "core/command_line.h"
#include "core/run_steps.h"

#include <catch_amalgamated.hpp>

#include <CLI11.hpp>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace {
    // The shared options and a map, as the programs set them up; parsed from words.
    struct Parsed {
        dx::cli::RunOptions options;
        std::string map;
        std::optional<int> exit;
    };

    Parsed parseWords(std::initializer_list<std::string> words)
    {
        Parsed parsed;
        parsed.options.width = 1280;
        parsed.options.height = 720;
        CLI::App app("test", "program");
        app.add_option("map", parsed.map);
        dx::cli::addRunOptions(app, parsed.options);
        std::vector<std::string> storage(words);
        std::vector<char*> argv { const_cast<char*>("program") };
        for (std::string& word : storage) {
            argv.push_back(word.data());
        }
        parsed.exit = dx::cli::parse(app, static_cast<int>(argv.size()), argv.data());
        return parsed;
    }

    // Runs steps for frames ticks, the level loaded from tick loadedFrom on; returns what ran on
    // each tick, "-" for none, and "quit" for the tick a quit came.
    std::vector<std::string> runTicks(std::vector<std::string> steps, int frames, int loadedFrom = 1)
    {
        dx::steps::Runner runner(std::move(steps));
        std::vector<std::string> log;
        for (int tick = 1; tick <= frames && !runner.quitting(); tick++) {
            std::string ran;
            runner.tick([&] { return tick >= loadedFrom; }, [&](const std::string& step) { ran += (ran.empty() ? "" : ",") + step; });
            log.push_back(runner.quitting() ? (ran.empty() ? "quit" : ran + ",quit") : (ran.empty() ? "-" : ran));
        }
        return log;
    }
}

TEST_CASE("the shared run options read a map, the size, the frames and the steps", "[cli]")
{
    Parsed parsed = parseWords({ "--hidden", "--frames", "30", "--do", "exec camera -600 0 0; wait 2", "00_Training", "--width", "800", "--do", "shot out.bmp;quit" });
    REQUIRE_FALSE(parsed.exit);
    CHECK(parsed.map == "00_Training");
    CHECK(parsed.options.hidden);
    CHECK(parsed.options.frames == 30);
    CHECK(parsed.options.width == 800);
    CHECK(parsed.options.height == 720);
    CHECK(parsed.options.sizeGiven);
    CHECK(parsed.options.steps == std::vector<std::string> { "exec camera -600 0 0", "wait 2", "shot out.bmp", "quit" });
    CHECK(parsed.options.automated());
}

TEST_CASE("--script reads a file of steps, in order with --do", "[cli]")
{
    std::filesystem::path file = std::filesystem::temp_directory_path() / "ambrosia-steps-test.txt";
    std::ofstream(file) << "# a comment\r\nexec one\n\n  wait 1s  \nquit\n";
    Parsed parsed = parseWords({ "--do", "first", "--script", file.string(), "--do", "last" });
    std::filesystem::remove(file);
    REQUIRE_FALSE(parsed.exit);
    CHECK(parsed.options.steps == std::vector<std::string> { "first", "exec one", "wait 1s", "quit", "last" });
    CHECK(parseWords({ "--script", (std::filesystem::temp_directory_path() / "ambrosia-no-such-steps.txt").string() }).exit == 2);
}

TEST_CASE("the shared run options keep their defaults when none is given", "[cli]")
{
    Parsed parsed = parseWords({});
    REQUIRE_FALSE(parsed.exit);
    CHECK(parsed.options.frames == -1);
    CHECK_FALSE(parsed.options.sizeGiven);
    CHECK_FALSE(parsed.options.hidden);
    CHECK(parsed.options.steps.empty());
    CHECK_FALSE(parsed.options.automated());
    // Steps that never quit play in real time.
    CHECK_FALSE(parseWords({ "--do", "cheats" }).options.automated());
}

TEST_CASE("a bad command line exits with 2 and --help with 0", "[cli]")
{
    CHECK(parseWords({ "--frmes", "30" }).exit == 2);
    CHECK(parseWords({ "--frames" }).exit == 2);
    CHECK(parseWords({ "--frames", "-1" }).exit == 2);
    CHECK(parseWords({ "--width", "0" }).exit == 2);
    CHECK(parseWords({ "--do", "wait soon" }).exit == 2);
    CHECK(parseWords({ "--do", "wait -3" }).exit == 2);
    CHECK(parseWords({ "MapOne", "MapTwo" }).exit == 2);
    CHECK(parseWords({ "--help" }).exit == 0);
}

TEST_CASE("steps run after each frame, in order, until one waits", "[cli]")
{
    // wait 2 on tick 1 runs the next step on tick 3; wait 1s, 30 frames, from 3 to 33.
    std::vector<std::string> expected { "a,b", "-", "c,d" };
    expected.insert(expected.end(), 29, "-");
    expected.push_back("e,quit");
    CHECK(runTicks({ "a", "b", "wait 2", "c", "wait 0", "d", "wait 1s", "e", "quit" }, 100) == expected);
    // Nothing after a quit runs.
    CHECK(runTicks({ "quit", "a" }, 5) == std::vector<std::string> { "quit" });
    // Steps that run out leave the run going.
    CHECK(runTicks({ "a" }, 3) == std::vector<std::string> { "a", "-", "-" });
}

TEST_CASE("wait loaded holds the steps until the level is loaded", "[cli]")
{
    CHECK(runTicks({ "open", "wait loaded", "shot" }, 5, 3) == std::vector<std::string> { "open", "-", "shot", "-", "-" });
    // Already loaded, it does not wait at all.
    CHECK(runTicks({ "wait loaded", "a" }, 2) == std::vector<std::string> { "a", "-" });
}

TEST_CASE("steps split on semicolons inline and on lines in a file", "[cli]")
{
    CHECK(dx::steps::splitInline(" a ; b;;c ") == std::vector<std::string> { "a", "b", "c" });
    CHECK(dx::steps::splitLines("a\n# b\n\n c \r\nd") == std::vector<std::string> { "a", "c", "d" });
    CHECK_FALSE(dx::steps::check("wait 30"));
    CHECK_FALSE(dx::steps::check("wait 2s"));
    CHECK_FALSE(dx::steps::check("wait loaded"));
    CHECK_FALSE(dx::steps::check("waiting"));
    CHECK(dx::steps::check("wait"));
    CHECK(dx::steps::check("wait 2m"));
}

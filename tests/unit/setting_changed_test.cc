#include "audio/audio.h"
#include "test_paths.h"
#include "core/config.h"
#include "deusex/startup.h"
#include "engine/console.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

TEST_CASE("setting notices reach listeners in the order they subscribed", "[config]")
{
    dx::Ini ini;
    std::vector<std::string> heard;
    dx::Subscription first = dx::config::onSettingChanged([&](const dx::config::SettingChange& change) {
        CHECK(&change.ini == &ini);
        heard.push_back("first " + std::string(change.section) + " " + std::string(change.key));
    });
    dx::Subscription second;
    second = dx::config::onSettingChanged([&](const dx::config::SettingChange& change) {
        heard.push_back("second " + std::string(change.key));
        second.reset();
    });
    dx::config::settingChanged(ini, "Some.Section", "Key");
    dx::config::settingChanged(ini, "Some.Section", "Other");
    CHECK(heard == std::vector<std::string> { "first Some.Section Key", "second Key", "first Some.Section Other" });
    first.reset();
    heard.clear();
    dx::config::settingChanged(ini, "Some.Section", "Key");
    CHECK(heard.empty());
}

TEST_CASE("set ini: announces the setting by its section and key, and audio follows its own", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    std::vector<std::string> heard;
    dx::Subscription watch = dx::config::onSettingChanged([&](const dx::config::SettingChange& change) {
        CHECK(&change.ini == &dx::config::system());
        heard.push_back(std::string(change.section) + " " + std::string(change.key));
    });
    std::string original = dx::console::execute(nullptr, "get ini:Engine.Engine.AudioDevice MusicVolume");
    dx::console::execute(nullptr, "set ini:Engine.Engine.AudioDevice MusicVolume 51");
    REQUIRE(heard.size() == 1);
    CHECK(heard[0] == "Galaxy.GalaxyAudioSubsystem MusicVolume");
    // The audio library re-reads its section, so the volume is that of the ini at once.
    CHECK(dx::audio::volumes().music == Catch::Approx(51.0f / 255.0f));
    dx::console::execute(nullptr, "set ini:Engine.Engine.AudioDevice MusicVolume " + original);
    CHECK(dx::audio::volumes().music == Catch::Approx(std::stof(original) / 255.0f));
    // An unknown section is refused and announces nothing.
    heard.clear();
    CHECK_FALSE(dx::console::execute(nullptr, "set ini:No.Such.Thing Key 1").empty());
    CHECK(heard.empty());
}

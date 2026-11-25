#include "debugui/overlay.h"

#include "core/objects.h"
#include "core/profile.h"
#include "debugui/actor_marks.h"
#include "debugui/imgui_setup.h"
#include "debugui/input_tool.h"
#include "deusex/startup.h"
#include "engine/world.h"
#include "test_paths.h"
#include "debugui/performance_tool.h"
#include "debugui/render_tool.h"
#include "render/render_debug.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

TEST_CASE("Actor mark selectors match by name, class and property values", "[debugui][data]")
{
    dx::engine::initialize(dx::test::gameData());
    const char* map = "06_HongKong_WanChai_Garage";
    dx::objects::unloadPackage(dx::Name(map));
    auto world = std::make_unique<dx::World>();
    REQUIRE(world->loadMap(map));
    std::vector<dx::UObject*> flicker = dx::debugui::findActors(world.get(), "* LightType=LT_Flicker");
    CHECK(flicker.size() == 3);
    CHECK(dx::debugui::findActors(world.get(), "* lighttype=4").size() == 3);
    CHECK(dx::debugui::findActors(world.get(), "light21").size() == 1);
    CHECK(dx::debugui::findActors(world.get(), "Light LightType=LT_Flicker AmbientSound=HumLight1").size() == 1);
    CHECK(dx::debugui::findActors(world.get(), "Light").size() > flicker.size());
    CHECK(dx::debugui::findActors(world.get(), "* LightType").empty());
    CHECK(dx::debugui::findActors(world.get(), "").empty());
    CHECK(dx::debugui::addMark(world.get(), "* LightType=LT_Flicker") == "marked 3 actors for * LightType=LT_Flicker");
    CHECK(dx::debugui::addMark(world.get(), "* lighttype=lt_flicker").rfind("already marked", 0) == 0);
    CHECK(dx::debugui::removeMark("all") == "removed 1 marks");
    world.reset();
    dx::objects::unloadPackage(dx::Name(map));
}

namespace {
    dx::debugui::Tool tool(const std::string& name, bool openAtFirst, int* closed = nullptr)
    {
        dx::debugui::Tool made;
        made.menu = "Test";
        made.title = name;
        made.name = name;
        made.draw = [](const dx::debugui::Frame&) {};
        made.openAtFirst = openAtFirst;
        if (closed) {
            made.closed = [closed] {
                (*closed)++;
            };
        }
        return made;
    }
}

// The overlay's console control works before ImGui has started, as in a scripted run that opens
// tools before showing the overlay.
TEST_CASE("Debug overlay tools open and close by name", "[debugui]")
{
    dx::debugui::Overlay overlay(nullptr);
    int closed = 0;
    overlay.addTool(tool("first", true, &closed));
    overlay.addTool(tool("second", false));

    CHECK(overlay.isToolOpen("first"));
    CHECK_FALSE(overlay.isToolOpen("second"));
    CHECK(overlay.toolNames() == std::vector<std::string> { "first", "second" });

    std::string reply;
    REQUIRE(overlay.runCommand("open", "second", reply));
    CHECK(overlay.isToolOpen("second"));
    CHECK(reply.find("opened second") == 0);

    REQUIRE(overlay.runCommand("close", "first", reply));
    CHECK_FALSE(overlay.isToolOpen("first"));
    CHECK(closed == 1);
    // Closing a closed tool does not call its closer again.
    CHECK(overlay.setToolOpen("first", false));
    CHECK(closed == 1);

    REQUIRE(overlay.runCommand("open", "all", reply));
    CHECK(overlay.isToolOpen("first"));
    CHECK(overlay.isToolOpen("second"));
    REQUIRE(overlay.runCommand("close", "all", reply));
    CHECK_FALSE(overlay.isToolOpen("first"));
    CHECK_FALSE(overlay.isToolOpen("second"));
    CHECK(closed == 2);

    REQUIRE(overlay.runCommand("open", "nothing", reply));
    CHECK(reply.find("first second") != std::string::npos);
    CHECK_FALSE(overlay.setToolOpen("nothing", true));
    CHECK_FALSE(overlay.isToolOpen("nothing"));
}

TEST_CASE("Debug overlay switches and commands reach their tools", "[debugui]")
{
    dx::debugui::Overlay overlay(nullptr);
    bool markers = false;
    overlay.addSwitch("markers", [&markers](bool on) {
        markers = on;
    });
    overlay.addCommand("ray", [](const std::string& value) {
        return "ray: " + value;
    });
    CHECK(overlay.setSwitch("markers", true));
    CHECK(markers);
    CHECK_FALSE(overlay.setSwitch("unknown", true));
    std::string reply;
    CHECK(overlay.runCommand("ray", "none", reply));
    CHECK(reply == "ray: none");
    CHECK_FALSE(overlay.runCommand("unknown", "x", reply));
    // The overlay's own commands come first.
    CHECK(overlay.commandNames() == std::vector<std::string> { "open", "close", "scale", "ray" });
    CHECK(overlay.mode() == dx::debugui::Overlay::Mode::Hidden);
}

TEST_CASE("Debug overlay scale is auto or a factor in range", "[debugui]")
{
    dx::debugui::Overlay overlay(nullptr);
    std::string reply;
    CHECK(overlay.runCommand("scale", "2", reply));
    CHECK(reply == "overlay scale 2");
    CHECK(overlay.runCommand("scale", "auto", reply));
    CHECK(reply == "overlay scale auto");
    for (const char* bad : { "9", "0.1", "big", "" }) {
        CHECK(overlay.runCommand("scale", bad, reply));
        CHECK(reply.starts_with("DebugOverlay scale <"));
    }
    // Without a window there is no display to read: scale 1.
    CHECK(dx::debugui::autoScale(nullptr) == 1.0f);
    CHECK(dx::debugui::scaleChoices().front() == 1.0f);
}

namespace {
    bool has(const std::vector<std::string>& names, const std::string& name)
    {
        return std::find(names.begin(), names.end(), name) != names.end();
    }
}

// Hidden runs drive the Performance, Render and Input tools from the console alone.
TEST_CASE("Debug overlay performance, render and input tools take console control", "[debugui]")
{
    dx::debugui::Overlay overlay(nullptr);
    dx::debugui::addPerformanceTool(overlay);
    dx::debugui::addRenderTool(overlay);
    int reads = 0;
    dx::debugui::addInputTool(overlay, [&reads] {
        reads++;
        return dx::debugui::InputState {};
    });
    CHECK(overlay.toolNames() == std::vector<std::string> { "performance", "render", "input" });
    // Nothing is read or recorded until a tool is drawn or a command asks.
    CHECK(reads == 0);
    CHECK_FALSE(dx::profile::isRecording());

    std::string reply;
    REQUIRE(overlay.setSwitch("perf-record", true));
    CHECK(dx::profile::isRecording());
    REQUIRE(overlay.setSwitch("perf-record", false));
    CHECK_FALSE(dx::profile::isRecording());
    REQUIRE(overlay.runCommand("perf-reset", "now", reply));
    REQUIRE(overlay.runCommand("perf-record-for", "soon", reply));
    CHECK(reply.find("perf-record-for <seconds>") != std::string::npos);
    CHECK_FALSE(dx::profile::isRecording());
    REQUIRE(overlay.runCommand("perf-record-for", "2f", reply));
    CHECK(reply.find("recording 2 frames") == 0);
    // The file goes in the test's user data folder (test_environment.cc).
    std::filesystem::path written = reply.substr(reply.find("the file goes to ") + 17);
    std::filesystem::remove(written);
    CHECK(dx::profile::isRecording());
    // Three frame ends: the first starts the recording, then two whole frames; the file is
    // written and the recorder goes back off.
    dx::profile::endFrame();
    dx::profile::endFrame();
    dx::profile::endFrame();
    CHECK_FALSE(dx::profile::isRecording());
    CHECK(std::filesystem::is_regular_file(written));
    std::filesystem::remove(written);
    // A name after the length names the file.
    REQUIRE(overlay.runCommand("perf-record-for", "1f spot-a", reply));
    CHECK(reply.find("recording 1 frames") == 0);
    written = reply.substr(reply.find("the file goes to ") + 17);
    CHECK(written.filename() == "spot-a.csv");
    dx::profile::endFrame();
    dx::profile::endFrame();
    CHECK(std::filesystem::is_regular_file(written));
    std::filesystem::remove(written);
    REQUIRE(overlay.runCommand("perf-record-for", "1fx", reply));
    CHECK(reply.find("perf-record-for <seconds>") != std::string::npos);
    CHECK(has(overlay.switchNames(), "gpu-timers"));

    std::string file = dx::debugui::performanceFile("run");
    CHECK(std::filesystem::path(file).filename() == "run.csv");
    CHECK(std::filesystem::path(file).parent_path().filename() == "Profiles");
    CHECK(std::filesystem::path(dx::debugui::performanceFile("")).filename().string().rfind("Frames", 0) == 0);

    for (const char* stage : { "render-mirrors", "render-sky", "render-lightmap-animation", "render-procedural", "render-meshes", "render-translucent", "render-occlusion", "render-coronas", "render-post", "render-msaa", "render-ao", "render-fxaa", "render-gamma", "render-hud" }) {
        CHECK(has(overlay.switchNames(), stage));
    }
    REQUIRE(overlay.setSwitch("render-mirrors", false));
    REQUIRE(overlay.setSwitch("render-hud", false));
    CHECK_FALSE(dx::renderDebug.mirrors);
    CHECK_FALSE(dx::renderDebug.hudScripts);
    CHECK(dx::renderDebug.sky);
    REQUIRE(overlay.runCommand("render-reset", "now", reply));
    CHECK(dx::renderDebug.mirrors);
    CHECK(dx::renderDebug.hudScripts);
}

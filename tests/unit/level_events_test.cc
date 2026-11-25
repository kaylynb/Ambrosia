#include "core/objects.h"
#include "test_paths.h"
#include "deusex/startup.h"
#include "engine/level_events.h"
#include "engine/resources.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

#include <memory>
#include <string>
#include <vector>

using dx::levelevents::Listener;
using dx::levelevents::Stage;
using dx::levelevents::Subscription;

namespace {
    Listener recorder(std::vector<std::string>& heard, const std::string& name)
    {
        return {
            [&heard, name](dx::World&) { heard.push_back(name + " loaded"); },
            [&heard, name](dx::World&) { heard.push_back(name + " unloading"); },
        };
    }
}

TEST_CASE("level notices go by stage, loaded forwards and unloading backwards", "[level]")
{
    std::vector<std::string> heard;
    // Subscribed out of stage order on purpose.
    Subscription game = dx::levelevents::subscribe(Stage::Game, recorder(heard, "game"));
    Subscription audio = dx::levelevents::subscribe(Stage::Audio, recorder(heard, "audio"));
    Subscription engine = dx::levelevents::subscribe(Stage::Engine, recorder(heard, "engine"));
    Subscription audio2 = dx::levelevents::subscribe(Stage::Audio, recorder(heard, "audio2"));
    dx::World world;
    dx::levelevents::levelLoaded(world);
    CHECK(heard == std::vector<std::string> { "engine loaded", "audio loaded", "audio2 loaded", "game loaded" });
    heard.clear();
    dx::levelevents::levelUnloading(world);
    CHECK(heard == std::vector<std::string> { "game unloading", "audio2 unloading", "audio unloading", "engine unloading" });
    heard.clear();
    audio.reset();
    dx::levelevents::levelLoaded(world);
    CHECK(heard == std::vector<std::string> { "engine loaded", "audio2 loaded", "game loaded" });
}

TEST_CASE("a level listener may unsubscribe itself and others while being told", "[level]")
{
    std::vector<std::string> heard;
    Subscription later = dx::levelevents::subscribe(Stage::Game, recorder(heard, "later"));
    Subscription self;
    self = dx::levelevents::subscribe(Stage::Engine, { [&](dx::World&) {
                                                          heard.push_back("self loaded");
                                                          self.reset();
                                                          later.reset();
                                                      },
                                                         nullptr });
    dx::World world;
    dx::levelevents::levelLoaded(world);
    dx::levelevents::levelLoaded(world);
    CHECK(heard == std::vector<std::string> { "self loaded" });
}

TEST_CASE("a moved subscription keeps its place and an empty one gives up nothing", "[level]")
{
    std::vector<std::string> heard;
    Subscription moved;
    {
        Subscription first = dx::levelevents::subscribe(Stage::Render, recorder(heard, "render"));
        moved = std::move(first);
    }
    dx::World world;
    dx::levelevents::levelLoaded(world);
    CHECK(heard == std::vector<std::string> { "render loaded" });
    moved.reset();
    moved.reset();
    heard.clear();
    dx::levelevents::levelLoaded(world);
    CHECK(heard.empty());
}

TEST_CASE("a world announces its level once loaded and before it lets go of it", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));

    struct Heard {
        std::string what;
        const dx::ULevel* level = nullptr;
        bool player = false;
    };

    std::vector<Heard> heard;
    Subscription watch = dx::levelevents::subscribe(Stage::Game, {
                                                                     [&](dx::World& world) { heard.push_back({ "loaded", world.level, world.player() != nullptr }); },
                                                                     [&](dx::World& world) { heard.push_back({ "unloading", world.level, world.player() != nullptr }); },
                                                                 });
    const dx::ULevel* island = nullptr;
    const dx::ULevel* hq = nullptr;
    {
        dx::World world;
        CHECK_FALSE(world.loadMap("NoSuchMapAtAll"));
        CHECK(heard.empty());
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        island = world.level;
        REQUIRE(heard.size() == 1);
        CHECK(heard[0].what == "loaded");
        CHECK(heard[0].level == island);
        CHECK(heard[0].player);
        // Loading another map into the same world lets go of the first level before it.
        REQUIRE(world.loadMap("01_NYC_UNATCOHQ"));
        hq = world.level;
        REQUIRE(heard.size() == 3);
        CHECK(heard[1].what == "unloading");
        CHECK(heard[1].level == island);
        CHECK(heard[2].what == "loaded");
        CHECK(heard[2].level == hq);
    }
    REQUIRE(heard.size() == 4);
    CHECK(heard[3].what == "unloading");
    CHECK(heard[3].level == hq);
    // A world that never loaded a level has nothing to announce.
    {
        dx::World empty;
    }
    CHECK(heard.size() == 4);
    watch.reset();
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
}

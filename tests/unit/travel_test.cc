#include "core/objects.h"
#include "core/url.h"
#include "test_paths.h"
#include "core/vm.h"
#include "deusex/map_lookup.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/level_release.h"
#include "engine/props_engine.h"
#include "engine/props_extension.h"
#include "engine/resources.h"
#include "engine/savegame.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <cmath>
#include <memory>

namespace {
    bool boolFlag(dx::UObject* flagBase, const char* name)
    {
        dx::vm::Call call(flagBase, dx::Name("GetBool"));
        call.n(dx::Name(name));
        call.run();
        return *reinterpret_cast<uint32_t*>(call.returnValue()) != 0;
    }
}

TEST_CASE("travel URLs name the map World::loadMap opens", "[travel]")
{
    CHECK(dx::maplookup::mapName("02_NYC_BatteryPark") == "02_NYC_BatteryPark");
    CHECK(dx::maplookup::mapName("02_NYC_BatteryPark.dx#ToBatteryPark?Difficulty=1") == "02_NYC_BatteryPark");
    CHECK(dx::maplookup::mapName("02_NYC_BatteryPark?loadgame=3") == "02_NYC_BatteryPark");
    CHECK(dx::maplookup::mapName("Maps/DX.dx") == "DX");
    CHECK(dx::maplookup::mapName("?restart").empty());
}

TEST_CASE("a teleporter's portal is found when the URL also carries options", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
    {
        dx::World world;
        std::string url = dx::travelUrl("01_NYC_UNATCOIsland?Difficulty=1", "01_NYC_UNATCOHQ#Inside_UN_HQ", dx::TRAVEL_Relative);
        REQUIRE(url.find("#Inside_UN_HQ?") != std::string::npos);
        REQUIRE(world.loadMap(url));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        dx::Vector location = dx::actor::Location(player);
        CHECK(std::abs(location.x - -1945.5f) < 1.0f);
        CHECK(std::abs(location.y - 1551.9f) < 1.0f);
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
}

TEST_CASE("travel finds shipped maps and not missing ones before leaving a level", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    CHECK(dx::maplookup::exists("02_NYC_BatteryPark"));
    CHECK(dx::maplookup::exists("02_nyc_batterypark"));
    CHECK(dx::maplookup::exists("DX"));
    CHECK_FALSE(dx::maplookup::exists("nosuchmap"));
    CHECK_FALSE(dx::maplookup::exists(""));
}

TEST_CASE("packageFile answers from the loader's package index", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    std::filesystem::path map = dx::objects::packageFile(dx::Name("02_NYC_BatteryPark"));
    CHECK(map.filename().string() == "02_NYC_BatteryPark.dx");
    CHECK(dx::objects::packageFile(dx::Name("02_nyc_batterypark")) == map);
    CHECK(dx::objects::packageFile(dx::Name("nosuchmap")).empty());
    dx::objects::setPackageFile(dx::Name("TravelTestMap"), map);
    CHECK(dx::objects::packageFile(dx::Name("TravelTestMap")) == map);
    CHECK(dx::maplookup::exists("TravelTestMap"));
    dx::objects::setPackageFile(dx::Name("TravelTestMap"), {});
    CHECK(dx::objects::packageFile(dx::Name("TravelTestMap")).empty());
    CHECK_FALSE(dx::maplookup::exists("TravelTestMap"));
}

TEST_CASE("ClientTravel sends PreTravel to the player and the mission script", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    dx::World world;
    REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
    for (int i = 0; i < 30; i++) {
        world.tick(1.0f / 30.0f);
    }
    dx::UObject* player = world.player();
    REQUIRE(player != nullptr);
    dx::UObject* flagBase = dx::playerpawnext::FlagBase(player);
    REQUIRE(flagBase != nullptr);
    dx::UObject* mission = nullptr;
    for (dx::UObject* a : world.actors) {
        if (a && !a->destroyed && a->isA(dx::Name("MissionScript"))) {
            mission = a;
        }
    }
    REQUIRE(mission != nullptr);
    uint8_t* missionFlags = mission->propertyAddress(dx::Name("flags"));
    REQUIRE(missionFlags != nullptr);
    CHECK(*reinterpret_cast<dx::UObject**>(missionFlags) == flagBase);
    CHECK_FALSE(boolFlag(flagBase, "PlayerTraveling"));
    {
        dx::vm::Call call(player, dx::Name("ClientTravel"));
        call.s("02_NYC_BatteryPark").byte(0).b(true);
        call.run();
    }
    CHECK(boolFlag(flagBase, "PlayerTraveling"));
    CHECK(*reinterpret_cast<dx::UObject**>(missionFlags) == nullptr);
    CHECK(world.travelPending);
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("travel carries changed values and keeps defaults from overwriting the new player", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    dx::PendingTravel travel;
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        uint8_t* augmentations = player->propertyAddress(dx::Name("AugmentationSystem"));
        uint8_t* credits = player->propertyAddress(dx::Name("Credits"));
        REQUIRE(augmentations != nullptr);
        REQUIRE(credits != nullptr);
        *reinterpret_cast<dx::UObject**>(augmentations) = nullptr;
        *reinterpret_cast<int32_t*>(credits) = 1234;
        travel = world.collectTravel();
        dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    }
    dx::World next;
    next.incomingTravel = travel.take();
    REQUIRE(next.loadMap("01_NYC_UNATCOIsland"));
    dx::UObject* player = next.player();
    REQUIRE(player != nullptr);
    dx::UObject* augmentations = *reinterpret_cast<dx::UObject**>(player->propertyAddress(dx::Name("AugmentationSystem")));
    REQUIRE(augmentations != nullptr);
    CHECK_FALSE(augmentations->destroyed);
    CHECK(*reinterpret_cast<int32_t*>(player->propertyAddress(dx::Name("Credits"))) == 1234);
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("the travel string uses the original GetTravelString layout", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    dx::World world;
    REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
    for (int i = 0; i < 30; i++) {
        world.tick(1.0f / 30.0f);
    }
    dx::UObject* player = world.player();
    REQUIRE(player != nullptr);
    std::string text = world.travelString(player);
    std::string header = "Class=DeusEx.JCDentonMale Name=" + player->name.str() + " Ptr=";
    REQUIRE(text.rfind(header, 0) == 0);
    size_t open = text.find("\r\n{\r\n");
    size_t close = text.find("}\r\n");
    REQUIRE(open != std::string::npos);
    REQUIRE(close != std::string::npos);
    std::string block = text.substr(open, close - open);
    const char* expected[] = { "\r\nSkillSystem=", "\r\nKeyRing=", "\r\nBarkManager=", "\r\nThemeManager=", "\r\nFlagBase=" };
    for (const char* line : expected) {
        INFO(line);
        CHECK(block.find(line) != std::string::npos);
    }
    size_t keyRing = block.find("\r\nKeyRing=");
    std::string keyRingName = block.substr(keyRing + 10 + 9, block.find("\r\n", keyRing + 2) - keyRing - 19);
    CHECK(text.find("Name=" + keyRingName + " Ptr=") != std::string::npos);
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

// The game collects the travel data, then stores the level it leaves, which deletes the
// destroyed actors. A travelling property that still names one must read None afterwards, not
// point at the deleted actor.
TEST_CASE("travel data loses a destroyed actor that storing the departing level deletes", "[data][gc]")
{
    dx::test::PlayFreeing freeing;
    dx::test::PlayFreeing::requireReported();
    dx::test::SaveRoot saves("travel-destroyed");
    dx::savegame::deleteCurrent();
    dx::test::PackageGuard island("01_NYC_UNATCOIsland");
    dx::test::PackageGuard hq("01_NYC_UNATCOHQ");
    auto world = std::make_unique<dx::World>();
    REQUIRE(world->loadMap("01_NYC_UNATCOIsland"));
    world->tick(1.0f / 30.0f);
    dx::UObject* player = world->player();
    REQUIRE(player != nullptr);
    dx::UClass* boxClass = dx::objects::findClass("DeusEx.BoxSmall");
    REQUIRE(boxClass != nullptr);
    dx::UObject* box = world->spawn(boxClass, nullptr, dx::Name(), dx::actor::Location(player), {}, nullptr, true);
    REQUIRE(box != nullptr);
    uint32_t boxHandle = box->handle;
    REQUIRE(world->destroy(box));
    uint8_t* augmentations = player->propertyAddress(dx::Name("AugmentationSystem"));
    REQUIRE(augmentations != nullptr);
    *reinterpret_cast<dx::UObject**>(augmentations) = box;

    auto travel = world->collectTravel();
    dx::savegame::storeDepartingLevel(*world, "01_NYC_UNATCOHQ");
    REQUIRE(dx::objects::atHandle(boxHandle) == nullptr);
    bool found = false;
    for (dx::TravelActor& entry : travel.actors) {
        for (dx::TravelValue& value : entry.values) {
            if (entry.isPlayer && value.property->name == dx::Name("AugmentationSystem")) {
                found = true;
                CHECK(*reinterpret_cast<dx::UObject**>(value.data.data()) == nullptr);
            }
        }
    }
    CHECK(found);

    dx::savegame::selectLevel("01_NYC_UNATCOHQ");
    dx::extension::reset();
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    world.reset();
    dx::World next;
    next.incomingTravel = travel.take();
    REQUIRE(next.loadMap("01_NYC_UNATCOHQ"));
    REQUIRE(next.player() != nullptr);
    CHECK(*reinterpret_cast<dx::UObject**>(next.player()->propertyAddress(dx::Name("AugmentationSystem"))) == nullptr);
    dx::levelrelease::collect(&next);
    next.tick(1.0f / 30.0f);
}

TEST_CASE("leaving a level frees it and keeps what the player carried", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
    dx::levelrelease::setEnabled(true);
    dx::PendingTravel travel;
    uint32_t levelHandle = 0;
    uint32_t playerHandle = 0;
    // Packages without a level that other tests unloaded stay unloaded and are not counted.
    size_t unloaded = 0;
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        // Whatever earlier tests unloaded goes here, so the counts below are this level's.
        dx::levelrelease::collect(&world);
        unloaded = dx::objects::linkerStats().retired;
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        dx::vm::Call note(player, dx::Name("AddNote"));
        note.s("remember this").b(true).b(false);
        note.run();
        REQUIRE(*reinterpret_cast<dx::UObject**>(player->propertyAddress(dx::Name("FirstNote"))) != nullptr);
        levelHandle = world.level->handle;
        playerHandle = player->handle;
        travel = world.collectTravel();
        dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    }
    {
        dx::World next;
        next.incomingTravel = travel.take();
        REQUIRE(next.loadMap("01_NYC_UNATCOHQ"));
        dx::World::releaseTravel(next.incomingTravel);
        // Travel itself moved the carried note under the new player (World::rehomeCarried).
        dx::UObject* carried = *reinterpret_cast<dx::UObject**>(next.player()->propertyAddress(dx::Name("FirstNote")));
        REQUIRE(carried != nullptr);
        CHECK(carried->outer == next.player());
        dx::levelrelease::Result result = dx::levelrelease::collect(&next);
        CHECK(result.packages >= 1);
        CHECK(result.pinned == 0);
        CHECK(result.freed > 2000);
        CHECK(result.kept < 50);
        CHECK(dx::objects::atHandle(levelHandle) == nullptr);
        CHECK(dx::objects::atHandle(playerHandle) == nullptr);
        CHECK(dx::objects::linkerStats().retired == unloaded);

        dx::UObject* player = next.player();
        REQUIRE(player != nullptr);
        dx::UObject* kept = *reinterpret_cast<dx::UObject**>(player->propertyAddress(dx::Name("FirstNote")));
        REQUIRE(kept != nullptr);
        CHECK(*reinterpret_cast<std::string*>(kept->propertyAddress(dx::Name("text"))) == "remember this");
        CHECK(kept->outer == player);
        dx::UObject* flags = *reinterpret_cast<dx::UObject**>(player->propertyAddress(dx::Name("flagBase")));
        REQUIRE(flags != nullptr);
        for (int i = 0; i < 30; i++) {
            next.tick(1.0f / 30.0f);
        }

        // Switched off, nothing is freed and the package stays unloaded.
        dx::levelrelease::setEnabled(false);
        dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
        dx::levelrelease::Result off = dx::levelrelease::collect(nullptr);
        CHECK(off.freed == 0);
        CHECK(dx::objects::linkerStats().retired == unloaded + 1);
        dx::levelrelease::setEnabled(true);
    }
    // With the world gone the last level goes too.
    dx::levelrelease::Result last = dx::levelrelease::collect(nullptr);
    CHECK(last.packages >= 1);
    CHECK(last.freed > 1000);
}

#include "core/config.h"
#include "test_paths.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/props_deusex.h"
#include "engine/props_engine.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

namespace {
    std::unique_ptr<dx::World> loadWorld(const std::string& map)
    {
        dx::engine::initialize(dx::test::gameData());
        dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
        dx::objects::unloadPackage(dx::Name(map));
        auto world = std::make_unique<dx::World>();
        REQUIRE(world->loadMap(map));
        return world;
    }

    void unload(std::unique_ptr<dx::World>& world, const std::string& map)
    {
        dx::objects::unloadPackage(dx::Name(map));
        world.reset();
    }

    dx::UObject* findActor(dx::World& world, const char* name)
    {
        for (dx::UObject* a : world.actors) {
            if (a && !a->destroyed && a->name == dx::Name(name)) {
                return a;
            }
        }
        return nullptr;
    }

    template <typename T>
    T& prop(dx::UObject* object, const char* name)
    {
        dx::UProperty* property = object->cls->findProperty(dx::Name(name));
        REQUIRE(property != nullptr);
        return *reinterpret_cast<T*>(object->data + property->offset);
    }

    void setBoolProp(dx::UObject* object, const char* name, bool value)
    {
        dx::UProperty* property = object->cls->findProperty(dx::Name(name));
        REQUIRE(property != nullptr);
        property->fromText(object->data + property->offset, value ? "True" : "False");
    }
}

TEST_CASE("actors bind barks only through BarkBindName and conversations count their owners", "[data]")
{
    std::unique_ptr<dx::World> world = loadWorld("01_NYC_UNATCOIsland");
    std::map<dx::UObject*, int32_t> owners;
    int bound = 0;
    for (dx::UObject* a : world->actors) {
        if (!a || a->destroyed || !a->cls->findProperty(dx::Name("ConListItems"))) {
            continue;
        }
        std::string bindName = prop<std::string>(a, "BindName");
        std::string barkBindName = prop<std::string>(a, "BarkBindName");
        for (dx::UObject* item = prop<dx::UObject*>(a, "ConListItems"); item; item = prop<dx::UObject*>(item, "Next")) {
            dx::UObject* con = prop<dx::UObject*>(item, "con");
            REQUIRE(con != nullptr);
            owners[con]++;
            bound++;
            std::string owner = prop<std::string>(con, "conOwnerName");
            bool isBark = prop<dx::Name>(con, "conName").str().find("_Bark") != std::string::npos;
            INFO(a->name.str() << " " << prop<dx::Name>(con, "conName").str());
            if (isBark && !barkBindName.empty()) {
                CHECK(dx::iequals(owner, barkBindName));
            } else {
                CHECK(dx::iequals(owner, bindName));
            }
        }
    }
    CHECK(bound > 10);
    for (const auto& [con, count] : owners) {
        INFO(prop<dx::Name>(con, "conName").str());
        CHECK(prop<int32_t>(con, "ownerRefCount") == count);
    }
    unload(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("Rand keeps to the range of the C runtime's rand", "[data]")
{
    std::unique_ptr<dx::World> world = loadWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    REQUIRE(player != nullptr);
    int32_t highest = 0;
    for (int i = 0; i < 4000; i++) {
        dx::vm::Call call(player, dx::Name("Rand"));
        REQUIRE(call.valid());
        int32_t value = call.i(65536).result<int32_t>();
        CHECK(value >= 0);
        highest = std::max(highest, value);
    }
    CHECK(highest <= 32767);
    CHECK(highest > 30000);
    dx::vm::Call none(player, dx::Name("Rand"));
    CHECK(none.i(0).result<int32_t>() == 0);
    unload(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("a pawn's weapon goes wherever the pawn is", "[data]")
{
    std::unique_ptr<dx::World> world = loadWorld("01_NYC_UNATCOIsland");
    int checked = 0;
    for (int i = 0; i < 30; i++) {
        world->tick(1.0f / 30.0f);
    }
    for (dx::UObject* a : world->actors) {
        if (!a || a->destroyed || !dx::isPawn(a) || !dx::pawn::Weapon(a)) {
            continue;
        }
        INFO(a->name.str());
        dx::Vector offset = dx::actor::Location(dx::pawn::Weapon(a)) - dx::actor::Location(a);
        CHECK(offset.sizeSquared() < 0.01f);
        checked++;
    }
    CHECK(checked > 3);
    unload(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("IsValidEnemy ignores bIgnore as the original does", "[data]")
{
    std::unique_ptr<dx::World> world = loadWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::UObject* terrorist = findActor(*world, "Terrorist2");
    REQUIRE(player != nullptr);
    REQUIRE(terrorist != nullptr);
    setBoolProp(player, "bIgnore", true);
    dx::vm::Call call(terrorist, dx::Name("IsValidEnemy"));
    CHECK(call.obj(player).b(false).resultBool());
    setBoolProp(player, "bDetectable", false);
    dx::vm::Call hidden(terrorist, dx::Name("IsValidEnemy"));
    CHECK_FALSE(hidden.obj(player).b(false).resultBool());
    unload(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("a ScriptedPawn's tick leaves settled timers and PrePivot alone", "[data]")
{
    std::unique_ptr<dx::World> world = loadWorld("01_NYC_UNATCOIsland");
    dx::UObject* terrorist = findActor(*world, "Terrorist2");
    REQUIRE(terrorist != nullptr);
    world->tick(1.0f / 30.0f);

    // PrePivot moves only while PrePivotTime runs.
    dx::scriptedpawn::PrePivotTime(terrorist) = 0;
    dx::actor::PrePivot(terrorist) = { 0, 0, 5 };
    dx::scriptedpawn::DesiredPrePivot(terrorist) = { 0, 0, 0 };
    // An alliance set without a timer is kept.
    prop<float>(terrorist, "PotentialEnemyTimer") = 0;
    prop<dx::Name>(terrorist, "PotentialEnemyAlliance") = dx::Name("Player");
    // A negative fire timer is not a running one.
    dx::scriptedpawn::FireTimer(terrorist) = -1.0f;
    world->tick(1.0f / 30.0f);
    CHECK(dx::actor::PrePivot(terrorist).z == 5.0f);
    CHECK(prop<dx::Name>(terrorist, "PotentialEnemyAlliance") == dx::Name("Player"));
    CHECK(dx::scriptedpawn::FireTimer(terrorist) == -1.0f);

    // A running timer clears the alliance when it runs out.
    prop<float>(terrorist, "PotentialEnemyTimer") = 0.01f;
    world->tick(1.0f / 30.0f);
    CHECK(prop<float>(terrorist, "PotentialEnemyTimer") == 0.0f);
    CHECK(prop<dx::Name>(terrorist, "PotentialEnemyAlliance") == dx::Name());

    // The reload timer stops at zero instead of going on below it.
    if (dx::pawn::Weapon(terrorist)) {
        dx::scriptedpawn::ReloadTimer(terrorist) = 0.01f;
        world->tick(1.0f / 30.0f);
        world->tick(1.0f / 30.0f);
        CHECK(dx::scriptedpawn::ReloadTimer(terrorist) == 0.0f);
    }
    unload(world, "01_NYC_UNATCOIsland");
}

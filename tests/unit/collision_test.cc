#include "core/objects.h"
#include "test_paths.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

// Direct checks of physics.cc and collision.cc on a shipped map. The values are what the code
// gives today, kept as a net for later changes to these paths.

namespace {
    const char* MapName = "01_NYC_UNATCOIsland";
    const dx::Vector Column { -4760.57f, 10430.81f, 0.0f };

    std::unique_ptr<dx::World> loadIsland()
    {
        dx::engine::initialize(dx::test::gameData());
        dx::objects::unloadPackage(dx::Name(MapName));
        auto world = std::make_unique<dx::World>();
        REQUIRE(world->loadMap(MapName));
        REQUIRE(world->player() != nullptr);
        return world;
    }

    void unloadIsland(std::unique_ptr<dx::World>& world)
    {
        world.reset();
        dx::objects::unloadPackage(dx::Name(MapName));
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

    bool finite(const dx::Vector& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }
}

TEST_CASE("a box trace stops on the level floor", "[data]")
{
    std::unique_ptr<dx::World> world = loadIsland();
    dx::CheckResult hit;
    REQUIRE(world->lineCheck(hit, Column + dx::Vector { 0, 0, -200 }, Column + dx::Vector { 0, 0, -400 }, { 17, 17, 40 }, nullptr, dx::TRACE_World));
    CHECK(hit.actor == world->levelInfo);
    CHECK(hit.time == Catch::Approx(0.22f).margin(0.00001f));
    CHECK(hit.location.z == Catch::Approx(-244.0f).margin(0.001f));
    CHECK(hit.normal.x == 0.0f);
    CHECK(hit.normal.y == 0.0f);
    CHECK(hit.normal.z == 1.0f);
    unloadIsland(world);
}

TEST_CASE("a zero-length point trace that starts in solid hits at its start", "[data]")
{
    std::unique_ptr<dx::World> world = loadIsland();
    dx::Vector inside = Column + dx::Vector { 0, 0, -950 };
    dx::CheckResult hit;
    REQUIRE(world->pointCheck(hit, inside, {}, nullptr, false));
    // A trace that starts inside solid geometry and never moves hits at time zero.
    REQUIRE(world->bspLineCheck(hit, inside, inside, {}));
    CHECK(hit.time == 0.0f);
    CHECK(hit.location.x == inside.x);
    CHECK(hit.location.y == inside.y);
    CHECK(hit.location.z == inside.z);
    CHECK(hit.item == 0);
    unloadIsland(world);
}

TEST_CASE("box traces hit a mover from each side", "[data]")
{
    std::unique_ptr<dx::World> world = loadIsland();
    dx::UObject* mover = findActor(*world, "DeusExMover7");
    REQUIRE(mover != nullptr);
    dx::Vector target = dx::actor::Location(mover);
    CHECK(target.x == -6448.0f);
    CHECK(target.y == 1376.0f);
    CHECK(target.z == -128.0f);
    const std::pair<dx::Vector, float> expected[] = {
        { { -400, 0, 0 }, 0.86499995f },
        { { 0, 400, 0 }, 0.544999957f },
        { { 0, -400, 0 }, 0.86499995f },
        { { 0, 0, 400 }, 0.854999959f },
    };
    for (const auto& [offset, time] : expected) {
        dx::CheckResult hit;
        INFO(offset.x << " " << offset.y << " " << offset.z);
        REQUIRE(world->lineCheck(hit, target + offset, target, { 10, 10, 10 }, nullptr, dx::TRACE_Movers));
        CHECK(hit.actor == mover);
        CHECK(hit.time == Catch::Approx(time).margin(0.000001f));
    }
    unloadIsland(world);
}

TEST_CASE("a falling player speeds up under gravity and lands on the floor", "[data]")
{
    std::unique_ptr<dx::World> world = loadIsland();
    dx::UObject* player = world->player();
    REQUIRE(world->farMoveActor(player, Column + dx::Vector { 0, 0, -100 }, false, false));
    dx::actor::Velocity(player) = {};
    world->tick(0.0f);
    dx::setPhysics(player, dx::PHYS_Falling, nullptr);
    const std::pair<float, float> expected[] = {
        { -100.697937f, -36.4154015f },
        { -102.43956f, -68.0819702f },
        { -105.23674f, -99.7488251f },
        { -109.089478f, -131.415405f },
        { -113.997772f, -163.082245f },
        { -119.961624f, -194.74884f },
    };
    for (const auto& [z, velocityZ] : expected) {
        world->tick(1.0f / 30.0f);
        CHECK(dx::actor::Physics(player) == dx::PHYS_Falling);
        CHECK(dx::actor::Location(player).z == Catch::Approx(z).margin(0.0001f));
        CHECK(dx::actor::Velocity(player).z == Catch::Approx(velocityZ).margin(0.0001f));
    }
    for (int i = 0; i < 60; i++) {
        world->tick(1.0f / 30.0f);
    }
    CHECK(dx::actor::Physics(player) == dx::PHYS_Walking);
    CHECK(dx::actor::Location(player).z == Catch::Approx(-256.2f).margin(0.0001f));
    unloadIsland(world);
}

TEST_CASE("a walking player steps up onto a low box, walks off it and stops at a wall", "[data]")
{
    std::unique_ptr<dx::World> world = loadIsland();
    dx::UObject* player = world->player();
    for (int i = 0; i < 60; i++) {
        world->tick(1.0f / 30.0f);
    }
    dx::Vector start = dx::actor::Location(player);
    REQUIRE(start.z == Catch::Approx(-256.2f).margin(0.0001f));
    REQUIRE(dx::pawn::MaxStepHeight(player) == 25.0f);
    dx::UClass* boxClass = dx::objects::findClass("DeusEx.BoxSmall");
    dx::objects::flushLoads();
    dx::UObject* box = world->spawn(boxClass, nullptr, dx::Name(), start + dx::Vector { 0, 120, 0 }, {}, nullptr, true);
    REQUIRE(box != nullptr);
    // A step 16 units high: the box stands on the player's floor.
    dx::actor::CollisionHeight(box) = 8.0f;
    dx::actor::CollisionRadius(box) = 40.0f;
    dx::setPhysics(box, dx::PHYS_None, nullptr);
    REQUIRE(world->farMoveActor(box, { start.x, 10560.0f, start.z - dx::actor::CollisionHeight(player) + 8.0f }, false, true));
    dx::actor::bStasis(box) = 0;
    auto walk = [&](int frames) {
        for (int i = 0; i < frames; i++) {
            dx::actor::Acceleration(player) = { 0, dx::pawn::AccelRate(player), 0 };
            dx::performPhysics(world.get(), player, 1.0f / 30.0f);
        }
    };
    walk(10);
    CHECK(dx::actor::Location(player).y == Catch::Approx(10491.4883f).margin(0.001f));
    CHECK(dx::actor::Location(player).z == Catch::Approx(-256.2f).margin(0.0001f));
    walk(3);
    CHECK(dx::actor::Location(player).y == Catch::Approx(10523.4893f).margin(0.001f));
    CHECK(dx::actor::Location(player).z == Catch::Approx(-238.175079f).margin(0.0001f));
    CHECK(dx::actor::Base(player) == box);
    walk(12);
    CHECK(dx::actor::Location(player).y == Catch::Approx(10649.5244f).margin(0.001f));
    CHECK(dx::actor::Location(player).z == Catch::Approx(-256.1f).margin(0.0001f));
    CHECK(dx::actor::Base(player) == world->levelInfo);
    walk(10);
    CHECK(dx::actor::Location(player).y == Catch::Approx(10649.7051f).margin(0.001f));
    CHECK(dx::actor::Physics(player) == dx::PHYS_Walking);
    unloadIsland(world);
}

TEST_CASE("a spider pawn with no speed stays put instead of moving by 0/0", "[data]")
{
    std::unique_ptr<dx::World> world = loadIsland();
    dx::UObject* player = world->player();
    for (int i = 0; i < 60; i++) {
        world->tick(1.0f / 30.0f);
    }
    dx::Vector start = dx::actor::Location(player);
    dx::setPhysics(player, dx::PHYS_Spider, nullptr);
    dx::pawn::Floor(player) = { 0, 0, 1 };
    dx::actor::Velocity(player) = {};
    dx::actor::Acceleration(player) = {};
    dx::performPhysics(world.get(), player, 1.0f / 30.0f);
    dx::Vector after = dx::actor::Location(player);
    REQUIRE(finite(after));
    CHECK(after.x == start.x);
    CHECK(after.y == start.y);
    CHECK(std::abs(after.z - start.z) < 1.0f);
    CHECK(finite(dx::actor::Velocity(player)));
    unloadIsland(world);
}

// Both engines read at most 64 hull planes, then read the leaf's bounding box.
// This scan counts leaves in the shipped maps that exceed that limit.
TEST_CASE("count shipped BSP leaves with more than 64 hull planes", "[.][scan]")
{
    dx::engine::initialize(dx::test::gameData());
    size_t leaves = 0;
    size_t over = 0;
    size_t maps = 0;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(dx::test::gameData()) / "Maps")) {
        if (entry.path().extension() != ".dx") {
            continue;
        }
        std::string name = entry.path().stem().string();
        dx::Linker* linker = dx::objects::loadPackage(dx::Name(name));
        if (!linker) {
            continue;
        }
        maps++;
        const dx::Package& package = linker->file();
        for (int32_t ref = 1; ref <= static_cast<int32_t>(package.exports().size()); ref++) {
            if (package.classOf(ref) != "Model") {
                continue;
            }
            auto* model = dx::cast<dx::UModel>(linker->object(ref));
            dx::objects::ensureLoaded(model);
            dx::objects::flushLoads();
            if (!model) {
                continue;
            }
            for (const dx::BspNode& node : model->nodes) {
                if (node.collisionBound < 0) {
                    continue;
                }
                size_t count = 0;
                for (size_t i = static_cast<size_t>(node.collisionBound); i < model->leafHulls.size() && model->leafHulls[i] >= 0; i++) {
                    count++;
                }
                leaves++;
                if (count > 64) {
                    over++;
                    std::printf("%s %s: %zu planes\n", name.c_str(), model->name.c_str(), count);
                }
            }
        }
        dx::objects::unloadPackage(dx::Name(name));
    }
    std::printf("%zu maps, %zu leaf hulls, %zu over 64 planes\n", maps, leaves, over);
    CHECK(maps > 0);
}

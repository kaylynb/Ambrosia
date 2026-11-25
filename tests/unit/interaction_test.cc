#include "core/config.h"
#include "test_paths.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/console.h"
#include "engine/props_engine.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

// Player interaction scenarios driven by input against the shipped maps

namespace {
    struct Input {
        float forward = 0;
        float up = 0;
        uint8_t duck = 0;
        bool jump = false;
    };

    Input input;

    std::unique_ptr<dx::World> load(const char* map)
    {
        dx::engine::initialize(dx::test::gameData());
        dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
        dx::objects::unloadPackage(dx::Name(map));
        auto world = std::make_unique<dx::World>();
        world->inputHook = [](dx::UObject* player, float delta) {
            dx::console::readInput(player, delta);
            dx::playerpawn::aForward(player) += input.forward;
            dx::playerpawn::aUp(player) += input.up;
            dx::pawn::bDuck(player) = input.duck;
            if (input.jump) {
                dx::playerpawn::bPressedJump(player) = 1;
                input.jump = false;
            }
        };
        REQUIRE(world->loadMap(map));
        input = {};
        return world;
    }

    void release(std::unique_ptr<dx::World>& world, const char* map)
    {
        input = {};
        dx::console::resetInput(world->player());
        dx::objects::unloadPackage(dx::Name(map));
        world.reset();
    }

    void ticks(dx::World& world, int count)
    {
        for (int i = 0; i < count; i++) {
            world.tick(1.0f / 30.0f);
        }
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

    void teleport(dx::UObject* player, const dx::Vector& spot, int yaw)
    {
        dx::console::execute(player, "teleport " + std::to_string(spot.x) + " " + std::to_string(spot.y) + " " + std::to_string(spot.z) + " " + std::to_string(yaw));
    }

    void face(dx::UObject* player, int yaw, int pitch)
    {
        dx::actor::Rotation(player).yaw = yaw;
        dx::pawn::ViewRotation(player) = { pitch, yaw, 0 };
    }

    dx::UObject* spawn(dx::World& world, const char* cls, const dx::Vector& location)
    {
        return world.spawn(dx::objects::findClass(cls), nullptr, dx::Name(), location, {}, nullptr, false);
    }

    dx::UObject* carriedDecoration(dx::UObject* player)
    {
        dx::UProperty* carried = player->cls->findProperty(dx::Name("CarriedDecoration"));
        return *reinterpret_cast<dx::UObject**>(player->data + carried->offset);
    }

    void pickUp(dx::World& world, dx::UObject* player, dx::UObject* decoration)
    {
        dx::Vector spot = dx::actor::Location(decoration);
        dx::console::execute(player, "lookat " + std::to_string(spot.x) + " " + std::to_string(spot.y) + " " + std::to_string(spot.z));
        ticks(world, 5);
        dx::vm::Call(player, dx::Name("ParseRightClick")).run();
        ticks(world, 5);
    }

    // The top of a mover's brush below a point
    float moverTop(dx::World& world, dx::UObject* mover, float x, float y)
    {
        dx::CheckResult hit;
        dx::Vector from { x, y, dx::actor::Location(mover).z + 400 };
        world.lineCheck(hit, from, from + dx::Vector { 0, 0, -800 }, {}, nullptr, dx::TRACE_Movers);
        REQUIRE(hit.actor == mover);
        return from.z - 800 * hit.time;
    }

    // Open ground on UNATCO Island, faced along -x
    const dx::Vector arena { -4300, 10400, -259 };
    const int facingArena = 32768;
}

TEST_CASE("a lift carries the player and what they hold up and back down", "[data]")
{
    auto world = load("06_HongKong_MJ12lab");
    dx::UObject* player = world->player();
    dx::UObject* lift = findActor(*world, "ElevatorMover7");
    REQUIRE(lift != nullptr);
    float top = moverTop(*world, lift, 0, -32);
    teleport(player, { 0, -32, top + 46 }, 0);
    ticks(*world, 15);
    dx::UObject* box = spawn(*world, "DeusEx.BoxSmall", dx::actor::Location(player) + dx::Vector { 50, 0, 0 });
    REQUIRE(box != nullptr);
    ticks(*world, 15);
    pickUp(*world, player, box);
    REQUIRE(carriedDecoration(player) == box);
    float gap = dx::actor::Location(player).z - dx::actor::Location(lift).z;
    float startZ = dx::actor::Location(player).z;
    for (uint8_t key : { uint8_t { 1 }, uint8_t { 0 } }) {
        dx::vm::Call(lift, dx::Name("InterpolateTo")).byte(key).f(3.0f).run();
        float drift = 0;
        for (int i = 0; i < 120; i++) {
            world->tick(1.0f / 30.0f);
            drift = std::max(drift, std::abs(dx::actor::Location(player).z - dx::actor::Location(lift).z - gap));
        }
        CHECK(drift < 2.0f);
        CHECK(dx::actor::Base(player) == lift);
        CHECK(carriedDecoration(player) == box);
        if (key == 1) {
            CHECK(dx::actor::Location(player).z > startZ + 200.0f);
        }
    }
    release(world, "06_HongKong_MJ12lab");
}

TEST_CASE("objects resting on a lift ride it", "[data]")
{
    auto world = load("09_NYC_Dockyard");
    dx::UObject* lift = findActor(*world, "DeusExMover47");
    REQUIRE(lift != nullptr);
    float top = moverTop(*world, lift, 2496, 4360);
    dx::UObject* box = spawn(*world, "DeusEx.BoxSmall", { 2600, 4360, top + 60 });
    dx::UObject* crate = spawn(*world, "DeusEx.CrateUnbreakableSmall", { 2650, 4300, top + 60 });
    REQUIRE(box != nullptr);
    REQUIRE(crate != nullptr);
    ticks(*world, 60);
    REQUIRE(dx::actor::Base(box) == lift);
    REQUIRE(dx::actor::Base(crate) == lift);
    float boxGap = dx::actor::Location(box).z - dx::actor::Location(lift).z;
    float crateGap = dx::actor::Location(crate).z - dx::actor::Location(lift).z;
    for (uint8_t key : { uint8_t { 1 }, uint8_t { 0 } }) {
        dx::vm::Call(lift, dx::Name("InterpolateTo")).byte(key).f(3.0f).run();
        ticks(*world, 120);
        CHECK(dx::actor::Base(box) == lift);
        CHECK(dx::actor::Base(crate) == lift);
        CHECK(dx::actor::Location(box).z - dx::actor::Location(lift).z == Catch::Approx(boxGap).margin(1.0f));
        CHECK(dx::actor::Location(crate).z - dx::actor::Location(lift).z == Catch::Approx(crateGap).margin(1.0f));
    }
    release(world, "09_NYC_Dockyard");
}

TEST_CASE("a door swinging open never traps the player", "[data]")
{
    int tried = 0;
    int trapped = 0;
    for (int angle = 0; angle < 16; angle++) {
        for (float radius : { 30.0f, 60.0f }) {
            auto world = load("01_NYC_UNATCOHQ");
            dx::UObject* player = world->player();
            dx::UObject* door = findActor(*world, "DeusExMover13");
            REQUIRE(door != nullptr);
            dx::Vector hinge = dx::actor::Location(door);
            float a = static_cast<float>(angle) * 3.14159265f / 8.0f;
            dx::Vector spot { hinge.x + radius * std::cos(a), hinge.y + radius * std::sin(a), hinge.z };
            dx::CheckResult floor;
            world->lineCheck(floor, spot + dx::Vector { 0, 0, 60 }, spot + dx::Vector { 0, 0, -300 }, {}, nullptr, dx::TRACE_Level);
            spot.z = spot.z + 60 - 360 * floor.time + 44;
            if (floor.time >= 1.0f || !world->farMoveActor(player, spot, true, false)) {
                release(world, "01_NYC_UNATCOHQ");
                continue;
            }
            teleport(player, spot, 0);
            ticks(*world, 10);
            if ((dx::actor::Location(player) - spot).size2D() > 4) {
                release(world, "01_NYC_UNATCOHQ");
                continue;
            }
            tried++;
            dx::UProperty* locked = door->cls->findProperty(dx::Name("bLocked"));
            locked->fromText(door->data + locked->offset, "False");
            dx::vm::Call(door, dx::Name("Frob")).obj(player).obj(nullptr).run();
            ticks(*world, 90);
            bool free = false;
            for (int direction = 0; direction < 4 && !free; direction++) {
                dx::Vector start = dx::actor::Location(player);
                face(player, direction * 16384, 0);
                input.forward = 6000;
                ticks(*world, 10);
                input.forward = 0;
                free = (dx::actor::Location(player) - start).size2D() > 8;
            }
            trapped += free ? 0 : 1;
            release(world, "01_NYC_UNATCOHQ");
        }
    }
    CHECK(tried > 8);
    CHECK(trapped == 0);
}

TEST_CASE("walking steps over low objects and pushes the ones it cannot climb, up to mass 100", "[data]")
{
    struct Obstacle {
        const char* cls;
        bool stepsOver;
        bool moves;
    };

    for (Obstacle obstacle : { Obstacle { "DeusEx.BoxSmall", true, true }, Obstacle { "DeusEx.CrateUnbreakableSmall", false, true }, Obstacle { "DeusEx.CrateUnbreakableLarge", false, false } }) {
        INFO(obstacle.cls);
        auto world = load("01_NYC_UNATCOIsland");
        dx::UObject* player = world->player();
        teleport(player, arena, facingArena);
        ticks(*world, 10);
        dx::UObject* object = spawn(*world, obstacle.cls, arena + dx::Vector { -90, 0, 0 });
        REQUIRE(object != nullptr);
        ticks(*world, 30);
        dx::Vector objectStart = dx::actor::Location(object);
        float startZ = dx::actor::Location(player).z;
        float rise = 0;
        face(player, facingArena, 0);
        input.forward = 6000;
        for (int i = 0; i < 75; i++) {
            world->tick(1.0f / 30.0f);
            rise = std::max(rise, dx::actor::Location(player).z - startZ);
        }
        CHECK((rise > 5.0f) == obstacle.stepsOver);
        CHECK(((dx::actor::Location(object) - objectStart).size2D() > 10.0f) == obstacle.moves);
        release(world, "01_NYC_UNATCOIsland");
    }
}

TEST_CASE("a box stacked on a pushed crate rides along with it", "[data]")
{
    auto world = load("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    teleport(player, arena, facingArena);
    ticks(*world, 10);
    dx::UObject* bottom = spawn(*world, "DeusEx.CrateUnbreakableSmall", arena + dx::Vector { -90, 0, 0 });
    REQUIRE(bottom != nullptr);
    ticks(*world, 30);
    dx::UObject* top = spawn(*world, "DeusEx.BoxSmall", dx::actor::Location(bottom) + dx::Vector { 0, 0, 40 });
    REQUIRE(top != nullptr);
    ticks(*world, 30);
    REQUIRE(dx::actor::Base(top) == bottom);
    dx::Vector bottomStart = dx::actor::Location(bottom);
    dx::Vector offset = dx::actor::Location(top) - bottomStart;
    face(player, facingArena, 0);
    input.forward = 6000;
    ticks(*world, 60);
    input.forward = 0;
    ticks(*world, 30);
    CHECK((dx::actor::Location(bottom) - bottomStart).size2D() > 50.0f);
    CHECK(dx::actor::Base(top) == bottom);
    dx::Vector now = dx::actor::Location(top) - dx::actor::Location(bottom);
    CHECK((now - offset).size() < 0.5f);
    release(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("the player can jump onto a low crate", "[data]")
{
    auto world = load("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    teleport(player, arena, facingArena);
    ticks(*world, 10);
    dx::UObject* crate = spawn(*world, "DeusEx.CrateUnbreakableSmall", arena + dx::Vector { -70, 0, 0 });
    REQUIRE(crate != nullptr);
    ticks(*world, 30);
    face(player, facingArena, 0);
    input.forward = 6000;
    ticks(*world, 4);
    input.jump = true;
    bool landed = false;
    for (int i = 0; i < 30 && !landed; i++) {
        world->tick(1.0f / 30.0f);
        landed = dx::actor::Base(player) == crate && dx::actor::Physics(player) == dx::PHYS_Walking;
    }
    CHECK(landed);
    release(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("crouching lowers the player's collision while the feet stay on the floor", "[data]")
{
    auto world = load("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    teleport(player, arena, facingArena);
    ticks(*world, 10);
    float standing = dx::actor::CollisionHeight(player);
    float feet = dx::actor::Location(player).z - standing;
    input.duck = 1;
    ticks(*world, 30);
    CHECK(dx::actor::CollisionHeight(player) < standing * 0.5f);
    CHECK(dx::actor::Location(player).z - dx::actor::CollisionHeight(player) == Catch::Approx(feet).margin(0.5f));
    input.duck = 0;
    ticks(*world, 30);
    CHECK(dx::actor::CollisionHeight(player) == standing);
    CHECK(dx::actor::Location(player).z - dx::actor::CollisionHeight(player) == Catch::Approx(feet).margin(0.5f));
    release(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("short falls are harmless and long ones hurt", "[data]")
{
    for (float drop : { 150.0f, 800.0f }) {
        INFO(drop);
        auto world = load("01_NYC_UNATCOIsland");
        dx::UObject* player = world->player();
        teleport(player, arena + dx::Vector { 0, 0, drop }, facingArena);
        int health = dx::pawn::Health(player);
        ticks(*world, 120);
        CHECK(dx::actor::Physics(player) == dx::PHYS_Walking);
        CHECK((dx::pawn::Health(player) < health) == (drop > 300.0f));
        release(world, "01_NYC_UNATCOIsland");
    }
}

TEST_CASE("the player climbs down the satcom hatch ladder from the top without falling", "[data]")
{
    auto world = load("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    for (dx::UObject* a : world->actors) {
        if (a && !a->destroyed && dx::actor::Tag(a) == dx::Name("satcomhatch")) {
            dx::vm::Call(a, dx::Name("Trigger")).obj(player).obj(player).run();
        }
    }
    ticks(*world, 60);
    teleport(player, { -6384, 1330, -70 }, 16384);
    ticks(*world, 10);
    int health = dx::pawn::Health(player);
    face(player, 16384, 55000);
    input.forward = 6000;
    float lowest = dx::actor::Location(player).z;
    for (int i = 0; i < 150; i++) {
        world->tick(1.0f / 30.0f);
        lowest = std::min(lowest, dx::actor::Location(player).z);
    }
    CHECK(lowest < -230.0f);
    CHECK(dx::pawn::Health(player) == health);
    release(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("the player swims after falling into water and surfaces holding jump", "[data]")
{
    auto world = load("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::UObject* water = findActor(*world, "WaterZone1");
    REQUIRE(water != nullptr);
    dx::Vector surface { -2802, 6641, -480 };
    REQUIRE(world->zoneAt(surface, nullptr, nullptr) == water);
    while (world->zoneAt(surface, nullptr, nullptr) == water && surface.z < 0) {
        surface.z += 1;
    }
    REQUIRE(surface.z < 0);
    teleport(player, surface + dx::Vector { 0, 0, 80 }, 0);
    ticks(*world, 60);
    CHECK(dx::actor::Physics(player) == dx::PHYS_Swimming);
    CHECK(dx::actor::Location(player).z + dx::pawn::EyeHeight(player) < surface.z);
    face(player, 0, 0);
    input.up = 6000;
    ticks(*world, 60);
    // At the surface the player bobs, falling briefly each time the body leaves
    // the water, so the check covers a few bobs rather than one tick.
    int swimming = 0;
    float highestEye = -100000.0f;
    float highestBody = -100000.0f;
    for (int i = 0; i < 30; i++) {
        ticks(*world, 1);
        swimming += dx::actor::Physics(player) == dx::PHYS_Swimming ? 1 : 0;
        CHECK((dx::actor::Physics(player) == dx::PHYS_Swimming || dx::actor::Physics(player) == dx::PHYS_Falling));
        highestEye = std::max(highestEye, dx::actor::Location(player).z + dx::pawn::EyeHeight(player));
        highestBody = std::max(highestBody, dx::actor::Location(player).z);
    }
    CHECK(swimming > 15);
    CHECK(highestEye > surface.z);
    CHECK(highestBody < surface.z + 8.0f);
    release(world, "01_NYC_UNATCOIsland");
}

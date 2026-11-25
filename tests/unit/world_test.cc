#include "core/objects.h"
#include "test_paths.h"
#include "core/vm.h"
#include "deusex/deusex.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/ai.h"
#include "engine/console.h"
#include "engine/props_deusex.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/savegame.h"
#include "engine/world.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace {
    dx::UObject* findActor(dx::World& world, const char* name)
    {
        for (dx::UObject* a : world.actors) {
            if (a && !a->destroyed && a->name == dx::Name(name)) {
                return a;
            }
        }
        return nullptr;
    }
}

TEST_CASE("hostile terrorist acquires and shoots the player", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    REQUIRE(player != nullptr);
    dx::UObject* terrorist = findActor(world, "Terrorist1");
    REQUIRE(terrorist != nullptr);
    dx::console::execute(player, "teleport -1900 -653 -95 32768");
    CHECK(dx::pawn::Enemy(terrorist) == nullptr);
    CHECK(dx::pawn::Health(player) == 100);
    bool acquired = false;
    for (int i = 0; i < 450; i++) {
        world.tick(1.0f / 30.0f);
        if (dx::pawn::Enemy(terrorist) == player) {
            acquired = true;
        }
    }
    CHECK(acquired);
    CHECK(dx::pawn::Health(player) < 100);
}

TEST_CASE("level tick measures each dynamic actor's distance from the player", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    REQUIRE(player != nullptr);
    dx::UObject* paul = findActor(world, "PaulDenton0");
    REQUIRE(paul != nullptr);
    dx::Vector origin = dx::actor::Location(player);
    dx::actor::Location(paul) = { origin.x + 3.0f, origin.y + 4.0f, origin.z };
    dx::actor::DistanceFromPlayer(paul) = 9999.0f;
    world.tick(1.0f / 30.0f);
    CHECK(dx::actor::DistanceFromPlayer(paul) == Catch::Approx(5.0f));
}

TEST_CASE("with steady frame timing, frames shorter than five milliseconds advance actors in real time", "[data]")
{
    struct SteadyTiming {
        SteadyTiming()
        {
            dx::frameTiming = dx::FrameTiming::Steady;
        }

        ~SteadyTiming()
        {
            dx::frameTiming = dx::FrameTiming::Original;
        }
    } steady;

    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    REQUIRE(player != nullptr);
    dx::UClass* crateClass = dx::objects::findClass("DeusEx.CrateUnbreakableSmall");
    dx::objects::flushLoads();
    REQUIRE(crateClass != nullptr);
    auto travelled = [&](float height, float step, int steps) {
        dx::Vector start = dx::actor::Location(player) + dx::Vector { 0, 0, height };
        dx::UObject* crate = world.spawn(crateClass, nullptr, dx::Name(), start, {}, nullptr, true);
        REQUIRE(crate != nullptr);
        dx::actor::bCollideWorld(crate) = 0;
        dx::actor::bCollideActors(crate) = 0;
        dx::actor::Physics(crate) = dx::PHYS_Projectile;
        dx::actor::Velocity(crate) = { 100, 0, 0 };
        start = dx::actor::Location(crate);
        float before = world.timeSeconds;
        for (int i = 0; i < steps; i++) {
            world.tick(step);
        }
        CHECK(world.timeSeconds - before == Catch::Approx(1.0f).epsilon(0.01));
        return (dx::actor::Location(crate) - start).size();
    };
    float coarse = travelled(200.0f, 0.01f, 100);
    float fine = travelled(400.0f, 0.001f, 1000);
    CHECK(coarse == Catch::Approx(100.0f).epsilon(0.02));
    CHECK(fine == Catch::Approx(100.0f).epsilon(0.02));
}

TEST_CASE("LastRendered reports time since the actor was drawn and never goes negative", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    world.tick(1.0f / 30.0f);
    dx::UObject* paul = findActor(world, "PaulDenton0");
    REQUIRE(paul != nullptr);
    dx::actor::LastRenderTime(paul) = world.timeSeconds - 2.0f;
    {
        dx::vm::Call call(paul, dx::Name("LastRendered"));
        call.run();
        CHECK(*reinterpret_cast<float*>(call.returnValue()) == Catch::Approx(2.0f));
    }
    dx::actor::LastRenderTime(paul) = world.timeSeconds + 10.0f;
    dx::vm::Call call(paul, dx::Name("LastRendered"));
    call.run();
    CHECK(*reinterpret_cast<float*>(call.returnValue()) == 0.0f);
}

TEST_CASE("scripted pawns run the native AI timer and fear decay every tick", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* terrorist = findActor(world, "Terrorist1");
    REQUIRE(terrorist != nullptr);
    dx::scriptedpawn::FearTimer(terrorist) = 0.0f;
    dx::scriptedpawn::FearDecayRate(terrorist) = 0.4f;
    dx::scriptedpawn::FearLevel(terrorist) = 1.0f;
    dx::scriptedpawn::TakeHitTimer(terrorist) = 2.0f;
    dx::scriptedpawn::PotentialEnemyTimer(terrorist) = 0.1f;
    dx::scriptedpawn::PotentialEnemyAlliance(terrorist) = dx::Name("Player");
    world.tick(0.25f);
    CHECK(dx::scriptedpawn::FearLevel(terrorist) == Catch::Approx(0.9f));
    CHECK(dx::scriptedpawn::TakeHitTimer(terrorist) == Catch::Approx(1.75f));
    CHECK(dx::scriptedpawn::PotentialEnemyTimer(terrorist) == 0.0f);
    CHECK(dx::scriptedpawn::PotentialEnemyAlliance(terrorist) == dx::Name());
}

TEST_CASE("actors that are idle and unseen for five seconds enter stasis", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    for (int i = 0; i < 25; i++) {
        world.tick(0.25f);
    }
    REQUIRE(world.timeSeconds > 6.0f);
    dx::UObject* boat = findActor(world, "NYPoliceBoat0");
    REQUIRE(boat != nullptr);
    dx::actor::bStasis(boat) = 1;
    dx::actor::Physics(boat) = dx::PHYS_None;
    dx::actor::LastRenderTime(boat) = 0.0f;
    auto inStasis = [&]() {
        dx::vm::Call call(boat, dx::Name("InStasis"));
        call.run();
        return *reinterpret_cast<uint32_t*>(call.returnValue()) != 0;
    };
    CHECK(inStasis());
    dx::actor::LastRenderTime(boat) = world.timeSeconds - 1.0f;
    CHECK_FALSE(inStasis());
    dx::actor::LastRenderTime(boat) = 0.0f;
    dx::actor::Physics(boat) = dx::PHYS_Falling;
    CHECK_FALSE(inStasis());
    dx::actor::bForceStasis(boat) = 1;
    CHECK(inStasis());
    dx::actor::bStasis(boat) = 0;
    CHECK_FALSE(inStasis());
}

TEST_CASE("box traces against level geometry stop short by a tenth of their length", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::CheckResult hit;
    REQUIRE(world.bspLineCheck(hit, { -4760.57f, 10430.81f, -256.25f }, { -4760.57f, 10430.81f, -283.25f }, { 20, 20, 43 }));
    CHECK(hit.time == Catch::Approx(4.75f / 27.0f - 0.1f).margin(0.0005f));
    REQUIRE(world.bspLineCheck(hit, { -4760.57f, 10430.81f, -256.25f }, { -4760.57f, 10430.81f, -356.25f }, {}));
    CHECK(hit.location.z == Catch::Approx(-303.5f).margin(0.01f));
}

TEST_CASE("pawns settle on the floor at the heights the original game saves", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    world.tick(0.4f);
    for (int i = 0; i < 150; i++) {
        world.tick(1.0f / 30.0f);
    }
    const std::pair<const char*, float> expected[] = {
        { "JCDentonMale0", -256.250f },
        { "PaulDenton0", -256.225f },
        { "UNATCOTroop1", -256.200f },
        { "Terrorist0", -92.200f },
        { "Terrorist3", -416.200f },
        { "GuntherHermann0", -120.065f },
        { "SecurityBot0", -245.445f },
        { "ThugMale3", 1087.800f },
    };
    for (const auto& [name, z] : expected) {
        dx::UObject* pawn = name == std::string_view("JCDentonMale0") ? world.player() : findActor(world, name);
        REQUIRE(pawn != nullptr);
        INFO(name);
        CHECK(dx::actor::Location(pawn).z == Catch::Approx(z).margin(0.002f));
    }
}

TEST_CASE("an NPC sees a near pawn and loses it beyond 4000 units unless distance is ignored", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    dx::UObject* paul = findActor(world, "PaulDenton0");
    REQUIRE(player != nullptr);
    REQUIRE(paul != nullptr);
    dx::pawn::Enemy(paul) = nullptr;
    dx::pawn::Visibility(player) = 128;
    dx::actor::Location(paul) = { -4760, 10430, -256 };
    dx::actor::Location(player) = { -3860, 10430, -256 };
    CHECK(dx::lineOfSightTo(paul, player, false, false));
    dx::actor::Location(player) = { -260, 10430, -256 };
    CHECK_FALSE(dx::lineOfSightTo(paul, player, false, false));
    CHECK(dx::lineOfSightTo(paul, player, false, true));
    dx::actor::Location(player) = { -4760, 6000, -256 };
    CHECK_FALSE(dx::lineOfSightTo(paul, player, false, true));
}

namespace {
    dx::UObject* spawnBox(dx::World& world, const dx::Vector& location, const char* className = "DeusEx.BoxSmall")
    {
        dx::UClass* cls = dx::objects::findClass(className);
        dx::objects::flushLoads();
        if (!cls) {
            return nullptr;
        }
        dx::UObject* box = world.spawn(cls, nullptr, dx::Name(), location, {}, nullptr, true);
        if (box) {
            dx::actor::bStasis(box) = 0;
        }
        return box;
    }

    dx::UObject* firstOfClass(dx::World& world, const char* className)
    {
        for (dx::UObject* a : world.actors) {
            if (a && !a->destroyed && a->isA(dx::Name(className))) {
                return a;
            }
        }
        return nullptr;
    }

    size_t indexOf(dx::World& world, dx::UObject* actor)
    {
        return static_cast<size_t>(std::find(world.actors.begin(), world.actors.end(), actor) - world.actors.begin());
    }

    dx::World* hookWorld = nullptr;
    dx::UObject* hookActor = nullptr;
    dx::NativeFunction hookOriginal = nullptr;
    int hookCalls = 0;
    bool hookSawActorInHash = false;

    void collisionHashProbe(dx::UObject* self, dx::Frame& stack, void* result)
    {
        if (self == hookActor) {
            hookCalls++;
            dx::Vector location = dx::actor::Location(hookActor);
            dx::Vector extent = dx::cylinderExtent(hookActor);
            const std::vector<dx::UObject*>& found = hookWorld->collisionHash.query(location - extent, location + extent);
            hookSawActorInHash = std::find(found.begin(), found.end(), hookActor) != found.end();
        }
        hookOriginal(self, stack, result);
    }
}

TEST_CASE("landing on a scripted pawn bounces the lander off instead of basing on it", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    dx::UObject* paul = findActor(world, "PaulDenton0");
    REQUIRE(player != nullptr);
    REQUIRE(paul != nullptr);
    dx::actor::Physics(player) = dx::PHYS_Falling;
    dx::actor::Velocity(player) = {};
    dx::setPhysics(player, dx::PHYS_Walking, paul);
    CHECK(dx::actor::Physics(player) == dx::PHYS_Falling);
    CHECK(dx::actor::Base(player) == nullptr);
    CHECK(dx::actor::Velocity(player).z == 50.0f);
    float sideways = dx::actor::Velocity(player).size2D();
    CHECK(sideways >= 25.0f);
    CHECK(sideways <= 50.0f);
}

TEST_CASE("switching to rotating physics clears velocity and acceleration", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* box = spawnBox(world, dx::actor::Location(world.player()) + dx::Vector { 0, 0, 300 });
    REQUIRE(box != nullptr);
    dx::actor::Physics(box) = dx::PHYS_Falling;
    dx::actor::Velocity(box) = { 10, 20, -30 };
    dx::actor::Acceleration(box) = { 1, 2, 3 };
    dx::setPhysics(box, dx::PHYS_Rotating, nullptr);
    CHECK(dx::actor::Velocity(box).zero());
    CHECK(dx::actor::Acceleration(box).zero());
}

TEST_CASE("a full touch list drops a non-player pawn to make room for a player", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    dx::UObject* node = firstOfClass(world, "PathNode");
    REQUIRE(node != nullptr);
    std::vector<dx::UObject*> pawns;
    for (dx::UObject* a : world.actors) {
        if (a && !a->destroyed && a->isA(dx::Name("ScriptedPawn")) && pawns.size() < 4) {
            pawns.push_back(a);
        }
    }
    REQUIRE(pawns.size() == 4);
    for (int i = 0; i < 4; i++) {
        dx::UObject* pawn = pawns[static_cast<size_t>(i)];
        dx::actor::Physics(pawn) = dx::PHYS_Walking;
        dx::actor::Touching(node, i) = pawn;
    }
    dx::beginTouch(node, player);
    CHECK(dx::actor::Touching(node, 0) == player);
    CHECK(dx::actor::Touching(node, 1) == pawns[1]);
    CHECK(dx::actor::Touching(node, 2) == pawns[2]);
    CHECK(dx::actor::Touching(node, 3) == pawns[3]);
}

TEST_CASE("a destroyed actor leaves the collision hash before its Destroyed event runs", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    world.tick(1.0f / 30.0f);
    dx::UObject* box = spawnBox(world, dx::actor::Location(world.player()) + dx::Vector { 200, 0, 0 }, "DeusEx.Trophy");
    REQUIRE(box != nullptr);
    REQUIRE(dx::actor::bCollideActors(box));
    dx::UFunction* function = dx::vm::findFunction(box, dx::Name("GetPlayerPawn"));
    REQUIRE(function != nullptr);
    REQUIRE(function->nativeIndex == 720);
    hookWorld = &world;
    hookActor = box;
    hookOriginal = dx::vm::bindNative(function);
    REQUIRE(hookOriginal != nullptr);
    hookCalls = 0;
    hookSawActorInHash = true;
    dx::vm::registerNative(720, collisionHashProbe);
    world.destroy(box);
    dx::vm::registerNative(720, hookOriginal);
    CHECK(hookCalls == 1);
    CHECK_FALSE(hookSawActorInHash);
}

TEST_CASE("an actor whose owner ticks later waits until the owner has ticked", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    dx::UObject* paul = findActor(world, "PaulDenton0");
    REQUIRE(paul != nullptr);
    world.tick(1.0f / 30.0f);
    dx::UObject* box = spawnBox(world, dx::actor::Location(player) + dx::Vector { 200, 0, 0 });
    REQUIRE(box != nullptr);
    REQUIRE(indexOf(world, paul) < indexOf(world, player));
    REQUIRE(indexOf(world, player) < indexOf(world, box));
    dx::actor::Owner(paul) = box;
    bool sampled = false;
    bool ownerTickedBeforePlayer = true;
    bool childTickedBeforePlayer = true;
    world.inputHook = [&](dx::UObject*, float delta) {
        if (delta > 0 && !sampled) {
            sampled = true;
            ownerTickedBeforePlayer = dx::actor::bTicked(box) == world.tickParity;
            childTickedBeforePlayer = dx::actor::bTicked(paul) == world.tickParity;
        }
    };
    world.tick(1.0f / 30.0f);
    world.inputHook = nullptr;
    REQUIRE(sampled);
    CHECK_FALSE(ownerTickedBeforePlayer);
    CHECK_FALSE(childTickedBeforePlayer);
    CHECK(dx::actor::bTicked(paul) == dx::actor::bTicked(player));
    CHECK(dx::actor::bTicked(box) == dx::actor::bTicked(player));
    dx::actor::Owner(paul) = nullptr;
}

TEST_CASE("actors spawned between frames are ticked on the next frame", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::Vector origin = dx::actor::Location(world.player());
    for (int frame = 0; frame < 2; frame++) {
        dx::UObject* box = spawnBox(world, origin + dx::Vector { 200.0f + 100.0f * static_cast<float>(frame), 0, 0 });
        REQUIRE(box != nullptr);
        dx::actor::LifeSpan(box) = 10.0f;
        world.tick(0.25f);
        INFO(frame);
        CHECK(dx::actor::LifeSpan(box) == Catch::Approx(9.75f));
    }
}

TEST_CASE("actors without physics do not rotate", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* box = spawnBox(world, dx::actor::Location(world.player()) + dx::Vector { 200, 0, 0 });
    REQUIRE(box != nullptr);
    dx::actor::Physics(box) = dx::PHYS_None;
    dx::actor::Rotation(box) = {};
    dx::actor::bFixedRotationDir(box) = 1;
    dx::actor::bRotateToDesired(box) = 0;
    dx::actor::RotationRate(box) = { 0, 6000, 0 };
    world.tick(0.1f);
    CHECK(dx::actor::Rotation(box).yaw == 0);
}

TEST_CASE("players-only mode stops non-player actors after their animation update", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* box = spawnBox(world, dx::actor::Location(world.player()) + dx::Vector { 200, 0, 0 });
    REQUIRE(box != nullptr);
    dx::actor::LifeSpan(box) = 10.0f;
    dx::levelinfo::bPlayersOnly(world.levelInfo) = 1;
    world.tick(0.25f);
    CHECK(dx::actor::LifeSpan(box) == 10.0f);
    dx::levelinfo::bPlayersOnly(world.levelInfo) = 0;
    world.tick(0.25f);
    CHECK(dx::actor::LifeSpan(box) == Catch::Approx(9.75f));
}

TEST_CASE("frames under 5 ms run the game fast with original timing and at real time with steady timing", "[data]")
{
    // ULevel::Tick raises every tick to at least 5 ms (docs/differences.md, "Frame-rate dependence").
    dx::engine::initialize(dx::test::gameData());
    for (dx::FrameTiming timing : { dx::FrameTiming::Original, dx::FrameTiming::Steady }) {
        dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* box = spawnBox(world, dx::actor::Location(world.player()) + dx::Vector { 200, 0, 0 });
        REQUIRE(box != nullptr);
        dx::actor::LifeSpan(box) = 10.0f;
        dx::frameTiming = timing;
        for (int i = 0; i < 10; i++) {
            world.tick(0.001f);
        }
        dx::frameTiming = dx::FrameTiming::Original;
        float passed = 10.0f - dx::actor::LifeSpan(box);
        CHECK(passed == Catch::Approx(timing == dx::FrameTiming::Steady ? 0.01f : 0.05f).margin(0.0005f));
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("with steady frame timing, electricity emitters tick 30 times a second whatever the frame rate", "[data]")
{
    // light_effects.h, SteadyEffectRate. Each tick the emitter's script adds the time it is
    // given to lastFlickerTime; a long flickerTime keeps it from resetting.
    dx::test::LoadedMap map("09_NYC_ShipBelow");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    REQUIRE(player != nullptr);
    dx::UObject* emitter = findActor(world, "ElectricityEmitter0");
    REQUIRE(emitter != nullptr);
    auto floatProperty = [&](const char* name) -> float& {
        dx::UProperty* property = emitter->cls->findProperty(dx::Name(name));
        REQUIRE(property != nullptr);
        return *reinterpret_cast<float*>(emitter->data + property->offset);
    };
    floatProperty("flickerTime") = 100.0f;
    float& lastFlickerTime = floatProperty("lastFlickerTime");
    // Close enough that the emitter does not freeze for being out of sight.
    dx::console::execute(player, "teleport -3637 1100 -236 0");
    world.tick(1.0f / 30.0f);

    struct RestoreTiming {
        ~RestoreTiming()
        {
            dx::frameTiming = dx::FrameTiming::Original;
        }
    } restore;
    const float frame144 = 1.0f / 144.0f;

    dx::frameTiming = dx::FrameTiming::Steady;
    world.effectTime = 0;
    lastFlickerTime = 0;
    for (int i = 0; i < 4; i++) {
        world.tick(frame144);
    }
    CHECK(lastFlickerTime == 0.0f);
    world.tick(frame144);
    CHECK(lastFlickerTime == Catch::Approx(1.0f / 30.0f));
    CHECK(world.effectTime == Catch::Approx(5 * frame144 - 1.0f / 30.0f).margin(1e-5));

    dx::frameTiming = dx::FrameTiming::Original;
    lastFlickerTime = 0;
    world.tick(frame144);
    CHECK(lastFlickerTime == Catch::Approx(frame144));
}

TEST_CASE("with steady frame timing, an electric beam keeps its bends until the effect clock steps", "[data]")
{
    // light_effects.h, SteadyEffectRate; the bends are drawn in LaserIterator's CurrentItem.
    dx::test::LoadedMap map("09_NYC_ShipBelow");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    REQUIRE(player != nullptr);
    dx::UObject* emitter = findActor(world, "ElectricityEmitter0");
    REQUIRE(emitter != nullptr);
    auto property = [&](const char* name) -> uint8_t* {
        dx::UProperty* found = emitter->cls->findProperty(dx::Name(name));
        REQUIRE(found != nullptr);
        return emitter->data + found->offset;
    };
    // No flicker, so the beam stays shown.
    *reinterpret_cast<float*>(property("flickerTime")) = 100.0f;
    *reinterpret_cast<uint32_t*>(property("bHiddenBeam")) = 0;
    dx::console::execute(player, "teleport -3637 1100 -236 0");

    struct RestoreTiming {
        ~RestoreTiming()
        {
            dx::frameTiming = dx::FrameTiming::Original;
        }
    } restore;
    auto bends = [&]() {
        std::vector<dx::Vector> points;
        dx::deusex::renderIterate(world, emitter, [&](dx::UObject* item) {
            points.push_back(dx::actor::Location(item));
        });
        return points;
    };

    dx::frameTiming = dx::FrameTiming::Steady;
    // The first draw makes the beam's LaserIterator, which the script's next trace fills in.
    bends();
    world.effectTime = 0;
    world.tick(1.0f / 30.0f);
    std::vector<dx::Vector> first = bends();
    REQUIRE(first.size() > 2);
    CHECK(bends() == first);
    world.tick(1.0f / 144.0f);
    CHECK(bends() == first);
    for (int i = 0; i < 4; i++) {
        world.tick(1.0f / 144.0f);
    }
    CHECK(bends() != first);

    dx::frameTiming = dx::FrameTiming::Original;
    world.tick(1.0f / 30.0f);
    std::vector<dx::Vector> original = bends();
    CHECK(bends() != original);
}

TEST_CASE("with steady frame timing, a laser confused by EMP flickers on the effect clock", "[data]")
{
    // light_effects.h, SteadyEffectRate. While confused, the trigger's script adds the time it
    // is given to confusionTimer and returns.
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* trigger = findActor(world, "LaserTrigger0");
    REQUIRE(trigger != nullptr);
    auto property = [&](const char* name) -> uint8_t* {
        dx::UProperty* found = trigger->cls->findProperty(dx::Name(name));
        REQUIRE(found != nullptr);
        return trigger->data + found->offset;
    };
    uint32_t& confused = *reinterpret_cast<uint32_t*>(property("bConfused"));
    float& confusionTimer = *reinterpret_cast<float*>(property("confusionTimer"));

    struct RestoreTiming {
        ~RestoreTiming()
        {
            dx::frameTiming = dx::FrameTiming::Original;
        }
    } restore;
    const float frame144 = 1.0f / 144.0f;

    dx::frameTiming = dx::FrameTiming::Steady;
    // Not confused: every tick.
    world.effectTime = 0;
    world.tick(frame144);
    CHECK(confusionTimer == 0.0f);
    CHECK(dx::deusex::onEffectClock(trigger) == false);

    confused = 1;
    confusionTimer = 0;
    world.effectTime = 0;
    CHECK(dx::deusex::onEffectClock(trigger));
    for (int i = 0; i < 4; i++) {
        world.tick(frame144);
    }
    CHECK(confusionTimer == 0.0f);
    world.tick(frame144);
    CHECK(confusionTimer == Catch::Approx(1.0f / 30.0f));

    dx::frameTiming = dx::FrameTiming::Original;
    confusionTimer = 0;
    world.tick(frame144);
    CHECK(confusionTimer == Catch::Approx(frame144));
}

TEST_CASE("spawned actors start as last rendered ten seconds before time zero", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* box = spawnBox(world, dx::actor::Location(world.player()) + dx::Vector { 200, 0, 0 });
    REQUIRE(box != nullptr);
    CHECK(dx::actor::LastRenderTime(box) == -10.0f);
}

TEST_CASE("level startup bases actors with an AttachTag on the actor carrying that tag", "[data]")
{
    dx::test::LoadedMap map("00_TrainingCombat");
    dx::World& world = map.world;
    dx::UObject* glass = findActor(world, "BreakableGlass0");
    REQUIRE(glass != nullptr);
    REQUIRE(dx::actor::AttachTag(glass) == dx::Name("guntarget1"));
    dx::UObject* base = dx::actor::Base(glass);
    REQUIRE(base != nullptr);
    CHECK(dx::actor::Tag(base) == dx::Name("guntarget1"));
}

TEST_CASE("level startup moves static actors ahead of dynamic ones and never ticks them", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    REQUIRE(world.firstDynamicActor > 2);
    REQUIRE(world.firstDynamicActor < world.actors.size());
    for (size_t i = 2; i < world.actors.size(); i++) {
        INFO(i);
        if (!world.actors[i]) {
            continue;
        }
        CHECK((dx::actor::bStatic(world.actors[i]) != 0) == (i < world.firstDynamicActor));
    }
    dx::UObject* staticActor = world.actors[2];
    dx::actor::LifeSpan(staticActor) = 10.0f;
    world.tick(0.25f);
    CHECK(dx::actor::LifeSpan(staticActor) == 10.0f);
}

TEST_CASE("a rolling actor on flat ground loses speed to ground and fluid friction", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* box = spawnBox(world, dx::actor::Location(world.player()) + dx::Vector { 0, 120, 0 });
    REQUIRE(box != nullptr);
    for (int i = 0; i < 60 && dx::actor::Physics(box) != dx::PHYS_None; i++) {
        world.tick(1.0f / 30.0f);
    }
    REQUIRE(dx::actor::Physics(box) == dx::PHYS_None);
    dx::UObject* zone = dx::region(box).zone;
    REQUIRE(zone != nullptr);
    REQUIRE(dx::zoneinfo::ZoneGroundFriction(zone) == 8.0f);
    REQUIRE(dx::zoneinfo::ZoneFluidFriction(zone) == 1.2f);
    REQUIRE(dx::zoneinfo::ZoneVelocity(zone).zero());
    dx::setPhysics(box, dx::PHYS_Rolling, nullptr);
    dx::actor::Velocity(box) = { 100, 0, 0 };
    dx::actor::Acceleration(box) = {};
    float startX = dx::actor::Location(box).x;
    dx::performPhysics(&world, box, 0.05f);
    CHECK(dx::actor::Physics(box) == dx::PHYS_Rolling);
    CHECK(dx::actor::Location(box).x - startX == Catch::Approx(2.82f).margin(0.001f));
    CHECK(dx::actor::Velocity(box).x == Catch::Approx(56.4f).margin(0.02f));
}

TEST_CASE("loading a saved level runs PostPostBeginPlay on its actors", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::SaveRoot saves("postpost");
    dx::test::PackageGuard island("01_NYC_UNATCOIsland");
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* script = firstOfClass(world, "MissionScript");
        REQUIRE(script != nullptr);
        REQUIRE(dx::actor::TimerRate(script) > 0.0f);
        dx::actor::TimerCounter(script) = 0.03125f;
        REQUIRE(dx::savegame::saveGame(world, 5, "postpost"));
        dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
        dx::objects::unloadPackage(dx::Name("SaveGame_01_NYC_UNATCOIsland"));
    }
    std::string map;
    REQUIRE(dx::savegame::prepareLoad(5, map));
    std::string url = map + "?loadgame=5";
    REQUIRE(dx::savegame::selectLevel(url));
    dx::World loaded;
    REQUIRE(loaded.loadMap(url));
    dx::UObject* script = firstOfClass(loaded, "MissionScript");
    REQUIRE(script != nullptr);
    CHECK(dx::actor::TimerCounter(script) == 0.0f);
}

namespace {
    std::unordered_map<dx::UObject*, int> attachCalls;
    dx::NativeFunction attachOriginal = nullptr;

    void countAttachDecal(dx::UObject* self, dx::Frame& stack, void* result)
    {
        attachCalls[self]++;
        attachOriginal(self, stack, result);
    }
}

TEST_CASE("script spawns give the new actor the spawner's instigator", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* paul = findActor(world, "PaulDenton0");
    dx::UObject* terrorist = findActor(world, "Terrorist1");
    REQUIRE(paul != nullptr);
    REQUIRE(terrorist != nullptr);
    dx::actor::Instigator(paul) = terrorist;
    dx::UClass* cls = dx::objects::findClass("DeusEx.BoxSmall");
    dx::objects::flushLoads();
    REQUIRE(cls != nullptr);
    dx::vm::Call spawn(paul, dx::Name("Spawn"));
    spawn.obj(cls).obj(paul);
    spawn.run();
    dx::UObject* box = *reinterpret_cast<dx::UObject**>(spawn.returnValue());
    REQUIRE(box != nullptr);
    CHECK(dx::actor::Owner(box) == paul);
    CHECK(dx::actor::Instigator(box) == terrorist);
}

TEST_CASE("traces ignore every actor up the source's owner chain but not its children", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::Vector origin = dx::actor::Location(world.player()) + dx::Vector { 0, 0, 200 };
    dx::UObject* source = spawnBox(world, origin);
    dx::UObject* middle = spawnBox(world, origin + dx::Vector { 0, 300, 0 });
    dx::UObject* top = spawnBox(world, origin + dx::Vector { 200, 0, 0 });
    REQUIRE(source != nullptr);
    REQUIRE(middle != nullptr);
    REQUIRE(top != nullptr);
    dx::actor::Owner(source) = middle;
    dx::actor::Owner(middle) = top;
    dx::CheckResult hit;
    world.lineCheck(hit, origin, origin + dx::Vector { 400, 0, 0 }, {}, source, dx::TRACE_Others);
    CHECK(hit.actor == nullptr);
    dx::Vector topLocation = dx::actor::Location(top);
    dx::Vector middleLocation = dx::actor::Location(middle);
    world.lineCheck(hit, topLocation, middleLocation, {}, top, dx::TRACE_Others);
    CHECK(hit.actor == middle);
}

TEST_CASE("an actor's owner still blocks its moves", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::Vector origin = dx::actor::Location(world.player()) + dx::Vector { 0, 0, 200 };
    dx::UObject* mover = spawnBox(world, origin);
    dx::UObject* owner = spawnBox(world, origin + dx::Vector { 100, 0, 0 });
    REQUIRE(mover != nullptr);
    REQUIRE(owner != nullptr);
    REQUIRE(dx::isBlockedBy(world, mover, owner));
    dx::actor::Owner(mover) = owner;
    dx::CheckResult hit;
    world.moveActor(mover, { 100, 0, 0 }, dx::actor::Rotation(mover), hit, false, false, false, false);
    CHECK(hit.actor == owner);
    CHECK(dx::actor::Location(mover).x < origin.x + 100.0f - dx::actor::CollisionRadius(owner));
}

TEST_CASE("turning a base swings the actors based on it the same way", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::Vector origin = dx::actor::Location(world.player()) + dx::Vector { 0, 0, 200 };
    dx::UObject* base = spawnBox(world, origin);
    dx::UObject* rider = spawnBox(world, origin + dx::Vector { 100, 0, 0 });
    REQUIRE(base != nullptr);
    REQUIRE(rider != nullptr);
    dx::actor::Physics(base) = dx::PHYS_None;
    dx::actor::Physics(rider) = dx::PHYS_None;
    dx::setCollision(rider, false, false, false);
    dx::actor::bCollideWorld(rider) = 0;
    dx::setBase(rider, base, false);
    REQUIRE(dx::actor::StandingCount(base) == 1);
    dx::Rotator turned = dx::actor::Rotation(base);
    turned.yaw += 16384;
    int32_t riderYaw = dx::actor::Rotation(rider).yaw;
    dx::CheckResult hit;
    world.moveActor(base, {}, turned, hit, false, false, false, false);
    dx::Vector offset = dx::actor::Location(rider) - dx::actor::Location(base);
    CHECK(offset.x == Catch::Approx(0.0f).margin(0.5f));
    CHECK(offset.y == Catch::Approx(100.0f).margin(0.5f));
    CHECK(dx::actor::Rotation(rider).yaw == riderYaw + 16384);
}

TEST_CASE("test moves update the actor's zone without zone events", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* box = spawnBox(world, dx::actor::Location(world.player()) + dx::Vector { 0, 0, 200 });
    REQUIRE(box != nullptr);
    dx::UObject* startZone = dx::region(box).zone;
    dx::UObject* otherZone = nullptr;
    dx::Vector otherLocation;
    for (dx::UObject* a : world.actors) {
        if (a && a->isA(dx::engineClasses().zoneInfo) && a != startZone && a != world.levelInfo && world.zoneAt(dx::actor::Location(a), nullptr, nullptr) == a) {
            otherZone = a;
            otherLocation = dx::actor::Location(a);
            break;
        }
    }
    REQUIRE(otherZone != nullptr);
    REQUIRE(world.farMoveActor(box, otherLocation, true, true));
    CHECK(dx::region(box).zone == otherZone);
    dx::Vector back = dx::actor::Location(world.player()) + dx::Vector { 0, 0, 200 };
    REQUIRE(world.farMoveActor(box, back, false, true));
    REQUIRE(dx::region(box).zone == startZone);
    dx::CheckResult hit;
    world.moveActor(box, otherLocation - back, dx::actor::Rotation(box), hit, true, false, false, true);
    CHECK(dx::region(box).zone == world.zoneAt(dx::actor::Location(box), nullptr, nullptr));
    CHECK(dx::region(box).zone != startZone);
}

TEST_CASE("actors spawned during level startup receive the later startup events too", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    dx::UClass* decal = dx::objects::findClass("Engine.Decal");
    dx::objects::flushLoads();
    REQUIRE(decal != nullptr);
    dx::UFunction* attach = decal->findFunction(dx::Name("AttachDecal"));
    REQUIRE(attach != nullptr);
    if (!attach->native) {
        attach->native = dx::vm::bindNative(attach);
    }
    attachOriginal = attach->native;
    attachCalls.clear();
    attach->native = countAttachDecal;
    dx::World world;
    bool loaded = world.loadMap("01_NYC_UNATCOIsland");
    attach->native = attachOriginal;
    REQUIRE(loaded);
    int shadows = 0;
    for (dx::UObject* a : world.actors) {
        if (a && !a->destroyed && a->isA(dx::Name("Shadow")) && dx::actor::Owner(a) && dx::actor::Owner(a)->isA(dx::Name("ScriptedPawn"))) {
            shadows++;
            INFO(a->name.str());
            CHECK(attachCalls[a] == 2);
        }
    }
    CHECK(shadows > 0);
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("paused frames zero the player's input axes and tick always-tick actors", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    dx::UObject* box = spawnBox(world, dx::actor::Location(player) + dx::Vector { 200, 0, 0 });
    REQUIRE(box != nullptr);
    dx::actor::bAlwaysTick(box) = 1;
    dx::actor::LifeSpan(box) = 10.0f;
    dx::UObject* other = spawnBox(world, dx::actor::Location(player) + dx::Vector { 300, 0, 0 });
    REQUIRE(other != nullptr);
    dx::actor::LifeSpan(other) = 10.0f;
    dx::levelinfo::Pauser(world.levelInfo) = "test";
    dx::playerpawn::aBaseY(player) = 5.0f;
    world.tick(0.25f);
    CHECK(world.paused);
    CHECK(dx::playerpawn::aBaseY(player) == 0.0f);
    CHECK(dx::actor::LifeSpan(box) == Catch::Approx(9.75f));
    CHECK(dx::actor::LifeSpan(other) == 10.0f);
}

TEST_CASE("paused frames tick an owned always-tick actor only when its owner's tick flag matches", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    dx::UObject* owner = spawnBox(world, dx::actor::Location(player) + dx::Vector { 200, 0, 0 });
    dx::UObject* child = spawnBox(world, dx::actor::Location(player) + dx::Vector { 300, 0, 0 });
    REQUIRE(owner != nullptr);
    REQUIRE(child != nullptr);
    dx::actor::Owner(child) = owner;
    dx::actor::bAlwaysTick(child) = 1;
    dx::actor::LifeSpan(child) = 10.0f;
    dx::levelinfo::Pauser(world.levelInfo) = "test";
    for (int frame = 0; frame < 4; frame++) {
        world.tick(0.25f);
        CHECK(world.newlySpawned.empty());
    }
    CHECK(dx::actor::LifeSpan(child) == Catch::Approx(9.5f));
    dx::actor::Owner(child) = nullptr;
}

TEST_CASE("a spider pawn with no surface around it falls", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* paul = findActor(world, "PaulDenton0");
    REQUIRE(paul != nullptr);
    REQUIRE(world.farMoveActor(paul, dx::actor::Location(paul) + dx::Vector { 0, 0, 600 }, false, true));
    dx::actor::Physics(paul) = dx::PHYS_Spider;
    dx::pawn::Floor(paul) = {};
    dx::actor::Velocity(paul) = { 10, 0, 0 };
    dx::performPhysics(&world, paul, 0.05f);
    CHECK(dx::actor::Physics(paul) == dx::PHYS_Falling);
}

TEST_CASE("physics hands the pending touch actor PostTouch and pops it off the chain", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::Vector origin = dx::actor::Location(world.player()) + dx::Vector { 0, 0, 200 };
    dx::UObject* a = spawnBox(world, origin);
    dx::UObject* b = spawnBox(world, origin + dx::Vector { 100, 0, 0 });
    dx::UObject* c = spawnBox(world, origin + dx::Vector { 200, 0, 0 });
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    dx::actor::Physics(a) = dx::PHYS_None;
    dx::actor::PendingTouch(a) = b;
    dx::actor::PendingTouch(b) = c;
    dx::performPhysics(&world, a, 0.05f);
    CHECK(dx::actor::PendingTouch(a) == c);
    CHECK(dx::actor::PendingTouch(b) == nullptr);
}

TEST_CASE("the player climbs the ladder under the satcom hatch when walking into its rungs", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    dx::World world;
    world.inputHook = [](dx::UObject* player, float delta) {
        dx::console::readInput(player, delta);
        dx::playerpawn::aForward(player) += 6000.0f;
    };
    REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
    dx::UObject* player = world.player();
    dx::UObject* hatch = nullptr;
    for (dx::UObject* a : world.actors) {
        if (a && !a->destroyed && dx::actor::Tag(a) == dx::Name("satcomhatch")) {
            hatch = a;
        }
    }
    REQUIRE(hatch != nullptr);
    dx::vm::Call(hatch, dx::Name("Trigger")).obj(player).obj(player).run();
    // the rungs sit between rails wider than the player, in front of a non-ladder wall
    dx::console::execute(player, "teleport -6384 1470 -280 16384");
    dx::pawn::ViewRotation(player).pitch = 8000;
    float startZ = dx::actor::Location(player).z;
    REQUIRE(startZ < -200.0f);
    for (int i = 0; i < 90; i++) {
        world.tick(1.0f / 30.0f);
    }
    CHECK(dx::actor::Location(player).z > -100.0f);
    dx::console::resetInput(player);
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("pawns level their pitch while walking, turn it at a fixed rate otherwise, and never roll", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* paul = findActor(world, "PaulDenton0");
    REQUIRE(paul != nullptr);
    REQUIRE(dx::actor::RotationRate(paul).pitch >= 4096);
    dx::actor::Physics(paul) = dx::PHYS_Walking;
    dx::actor::Velocity(paul) = {};
    dx::actor::Acceleration(paul) = {};
    dx::Rotator start = dx::actor::Rotation(paul);
    start.pitch = 0;
    start.roll = 1000;
    dx::actor::Rotation(paul) = start;
    dx::actor::DesiredRotation(paul) = { 4000, start.yaw, 0 };
    dx::performPhysics(&world, paul, 0.05f);
    CHECK(dx::actor::Rotation(paul).pitch == 0);
    CHECK(dx::actor::Rotation(paul).roll == 0);
    dx::actor::Physics(paul) = dx::PHYS_Flying;
    dx::performPhysics(&world, paul, 0.05f);
    CHECK(dx::actor::Rotation(paul).pitch == 1638);
}

TEST_CASE("the player's input is read again with -1 after PlayerTick, which zeroes the axes", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    std::vector<float> deltas;
    world.inputHook = [&](dx::UObject*, float delta) {
        deltas.push_back(delta);
    };
    world.tick(0.25f);
    world.inputHook = nullptr;
    REQUIRE(deltas.size() == 2);
    CHECK(deltas[0] == 0.25f);
    CHECK(deltas[1] == -1.0f);
    dx::playerpawn::aBaseY(player) = 5.0f;
    dx::console::readInput(player, -1.0f);
    CHECK(dx::playerpawn::aBaseY(player) == 0.0f);
}

TEST_CASE("a player pawn without a player object is blocked like any other actor", "[data]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOIsland");
    dx::World& world = map.world;
    dx::UObject* player = world.player();
    dx::UObject* box = spawnBox(world, dx::actor::Location(player) + dx::Vector { 0, 0, 300 });
    REQUIRE(box != nullptr);
    dx::actor::bBlockActors(box) = 1;
    dx::actor::bBlockPlayers(box) = 0;
    dx::UObject* possessing = dx::playerpawn::Player(player);
    dx::playerpawn::Player(player) = nullptr;
    CHECK(dx::isBlockedBy(world, player, box));
    dx::playerpawn::Player(player) = possessing ? possessing : box;
    CHECK_FALSE(dx::isBlockedBy(world, player, box));
    dx::playerpawn::Player(player) = possessing;
}

namespace {
    dx::UObject* spawnMedKit(dx::World& world, const dx::Vector& location)
    {
        return world.spawn(dx::objects::findClass("DeusEx.MedKit"), nullptr, dx::Name(), location, {}, nullptr, true);
    }

    dx::UObject*& regionZone(dx::UObject* a)
    {
        return *reinterpret_cast<dx::UObject**>(dx::actor::Region(a));
    }
}

TEST_CASE("references to a destroyed actor stay until 128 are destroyed, then go from the actor list only", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    // Made before the level, as an object carried from an earlier level would be.
    dx::UObject* earlier = dx::objects::newObject(dx::engineClasses().viewport, dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
    REQUIRE(earlier != nullptr);
    dx::World world;
    REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
    dx::UObject* player = world.player();
    REQUIRE(player != nullptr);
    dx::UObject* holder = nullptr;
    for (size_t i = 2; i < world.firstDynamicActor && !holder; i++) {
        if (world.actors[i] && dx::actor::bStatic(world.actors[i])) {
            holder = world.actors[i];
        }
    }
    REQUIRE(holder != nullptr);
    dx::UObject* zone = regionZone(holder);
    dx::Vector spot = dx::actor::Location(player) + dx::Vector { 0, 0, 200 };
    dx::UObject* victim = spawnMedKit(world, spot);
    REQUIRE(victim != nullptr);
    dx::actor::Instigator(holder) = victim;
    regionZone(holder) = victim;
    dx::player::Actor(earlier) = victim;
    REQUIRE(world.destroy(victim));
    world.tick(1.0f / 30.0f);
    // ULevel::CleanupDestroyed leaves them until 128 actors wait to be cleaned up.
    CHECK(dx::actor::Instigator(holder) == victim);
    CHECK(regionZone(holder) == victim);
    CHECK(dx::player::Actor(earlier) == victim);
    CHECK(std::find(world.actors.begin(), world.actors.end(), victim) == world.actors.end());
    for (int i = 0; i < 127; i++) {
        dx::UObject* extra = spawnMedKit(world, spot);
        REQUIRE(extra != nullptr);
        REQUIRE(world.destroy(extra));
    }
    world.tick(1.0f / 30.0f);
    CHECK(dx::actor::Instigator(holder) == nullptr);
    CHECK(regionZone(holder) == nullptr);
    // Cleanup only scans the level's actor list. This object is outside that list,
    // so it still holds the old pointer even if the actor has already been freed.
    CHECK(dx::player::Actor(earlier) == victim);
    dx::player::Actor(earlier) = nullptr;
    CHECK(world.destroyedActors.empty());

    dx::UObject* second = spawnMedKit(world, spot);
    REQUIRE(second != nullptr);
    dx::actor::Instigator(holder) = second;
    REQUIRE(world.destroy(second));
    world.tick(1.0f / 30.0f);
    CHECK(dx::actor::Instigator(holder) == second);
    // Saving and leaving the level force the clean-up.
    world.cleanupDestroyed(true);
    CHECK(dx::actor::Instigator(holder) == nullptr);
    regionZone(holder) = zone;
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("decal pieces follow a rebuilt model and go when the decal is destroyed", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
    dx::World world;
    REQUIRE(world.loadMap("01_NYC_UNATCOHQ"));
    dx::CheckResult floor;
    REQUIRE(world.bspLineCheck(floor, { -1985, 813, 598 }, { -1985, 813, 0 }, {}));
    dx::UClass* holeClass = dx::objects::findClass("DeusEx.BulletHole");
    dx::UObject* hole = world.spawn(holeClass, nullptr, dx::Name(), floor.location + dx::Vector { 0, 0, 8 }, { 16384, 0, 0 }, nullptr, true);
    REQUIRE(hole != nullptr);
    REQUIRE(world.decals.pieces.count(hole) == 1);
    dx::UModel* model = world.level->model;
    CHECK(world.decals.model == model);
    CHECK(world.decals.revision == model->revision);
    // A rebuild that keeps the surface count must not reuse the old node lists.
    std::vector<std::vector<int32_t>> fresh = world.decals.surfNodes;
    for (std::vector<int32_t>& nodes : world.decals.surfNodes) {
        nodes.clear();
    }
    model->revision++;
    dx::UObject* second = world.spawn(holeClass, nullptr, dx::Name(), floor.location + dx::Vector { 4, 0, 8 }, { 16384, 0, 0 }, nullptr, true);
    REQUIRE(second != nullptr);
    CHECK(world.decals.revision == model->revision);
    CHECK(world.decals.surfNodes == fresh);
    REQUIRE(world.decals.pieces.count(second) == 1);
    const dx::DecalPiece& first = world.decals.pieces[hole].front();
    const dx::DecalPiece& next = world.decals.pieces[second].front();
    CHECK(next.surf == first.surf);
    CHECK(next.nodes == first.nodes);
    CHECK_FALSE(world.decals.surfNodes[static_cast<size_t>(next.surf)].empty());
    REQUIRE(world.destroy(hole));
    CHECK(world.decals.pieces.count(hole) == 0);
    CHECK(world.decals.pieces.count(second) == 1);
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
}
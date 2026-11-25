#include "core/gc.h"
#include "test_paths.h"
#include "core/objects.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/ai.h"
#include "engine/decal.h"
#include "engine/editor_history.h"
#include "engine/mesh.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <string>
#include <typeinfo>
#include <vector>

namespace {
    bool alive(const dx::UObject* object, uint32_t handle)
    {
        return dx::objects::atHandle(handle) == object;
    }

    bool contains(const std::vector<dx::UObject*>& list, const dx::UObject* object)
    {
        return std::find(list.begin(), list.end(), object) != list.end();
    }

    // Everything the collision hash can return, whatever its cell.
    std::vector<dx::UObject*> everythingColliding(dx::World& world)
    {
        return world.collisionHash.query({ -1e6f, -1e6f, -1e6f }, { 1e6f, 1e6f, 1e6f });
    }
}

TEST_CASE("actors released mid-level leave nothing of themselves in the world", "[data][gc]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOHQ");
    dx::World& world = map.world;
    world.tick(1.0f / 30.0f);
    dx::UObject* player = world.player();
    REQUIRE(player != nullptr);

    dx::UClass* crateClass = dx::objects::findClass("DeusEx.CrateUnbreakableSmall");
    dx::UClass* holeClass = dx::objects::findClass("DeusEx.BulletHole");
    dx::objects::flushLoads();
    REQUIRE(crateClass != nullptr);
    REQUIRE(holeClass != nullptr);

    // Cover both normal cleanup of a destroyed crate and direct deletion of live objects.
    // The live crate is still in the collision hash and the decal is still registered.
    dx::Vector origin = dx::actor::Location(player);
    dx::UObject* destroyed = world.spawn(crateClass, nullptr, dx::Name(), origin + dx::Vector { 0, 0, 200 }, {}, nullptr, true);
    dx::UObject* crate = world.spawn(crateClass, nullptr, dx::Name(), origin + dx::Vector { 0, 0, 400 }, {}, nullptr, true);
    REQUIRE(destroyed != nullptr);
    REQUIRE(crate != nullptr);
    world.collisionHash.update(crate);
    REQUIRE(contains(everythingColliding(world), crate));

    dx::CheckResult floor;
    REQUIRE(world.bspLineCheck(floor, { -1985, 813, 598 }, { -1985, 813, 0 }, {}));
    dx::UObject* hole = world.spawn(holeClass, nullptr, dx::Name(), floor.location + dx::Vector { 0, 0, 8 }, { 16384, 0, 0 }, nullptr, true);
    REQUIRE(hole != nullptr);
    REQUIRE(world.decals.pieces.count(hole) == 1);

    REQUIRE(world.destroy(destroyed));
    REQUIRE(contains(world.destroyedActors, destroyed));

    // Travel data that names them, as a level change leaves it.
    dx::TravelActor carried;
    carried.cls = crateClass;
    carried.original = crate;
    world.incomingTravel.push_back(carried);

    uint32_t destroyedHandle = destroyed->handle;
    uint32_t crateHandle = crate->handle;
    uint32_t holeHandle = hole->handle;
    dx::gc::Result result = dx::gc::release({ destroyed, crate, hole });
    CHECK(result.freed == 3);
    CHECK_FALSE(alive(destroyed, destroyedHandle));
    CHECK_FALSE(alive(crate, crateHandle));
    CHECK_FALSE(alive(hole, holeHandle));

    for (const dx::UObject* gone : { destroyed, crate, hole }) {
        CHECK_FALSE(contains(world.actors, gone));
        CHECK_FALSE(contains(world.level->actors, gone));
        CHECK_FALSE(contains(world.destroyedActors, gone));
        CHECK_FALSE(contains(world.newlySpawned, gone));
        CHECK_FALSE(contains(everythingColliding(world), gone));
        CHECK(world.decals.pieces.count(const_cast<dx::UObject*>(gone)) == 0);
    }
    CHECK(std::find(world.destroyedActors.begin(), world.destroyedActors.end(), nullptr) == world.destroyedActors.end());
    REQUIRE(world.incomingTravel.size() == 1);
    CHECK(world.incomingTravel[0].original == nullptr);
    CHECK(world.incomingTravel[0].cls == crateClass);
    world.incomingTravel.clear();

    // The world carries on: the emptied slots are packed and nothing reaches for the freed
    // actors.
    for (int i = 0; i < 30; i++) {
        world.tick(1.0f / 30.0f);
    }
    world.cleanupDestroyed(true);
    CHECK(std::find(world.actors.begin() + 2, world.actors.end(), nullptr) == world.actors.end());
    CHECK(world.player() == player);
}

TEST_CASE("with a level loaded every engine object reports its references", "[data][gc]")
{
    dx::test::LoadedMap map("01_NYC_UNATCOHQ");
    map.world.tick(1.0f / 30.0f);
    std::vector<std::string> unreported = dx::gc::unreportedTypes();
    for (const std::type_info* type : { &typeid(dx::UPalette), &typeid(dx::UTexture), &typeid(dx::UFont), &typeid(dx::USound), &typeid(dx::UMusic), &typeid(dx::UPolys), &typeid(dx::UPrimitive), &typeid(dx::UModel),
             &typeid(dx::ULevel), &typeid(dx::UMesh), &typeid(dx::ULodMesh), &typeid(dx::UFireTexture), &typeid(dx::UAIEvent), &typeid(dx::UAISenderEvent), &typeid(dx::UAIReceiverEvent), &typeid(dx::UAIEventType),
             &typeid(dx::UEventManager) }) {
        INFO(type->name());
        CHECK(std::find(unreported.begin(), unreported.end(), std::string(type->name())) == unreported.end());
    }
}

TEST_CASE("the editor's undo history keeps what it could put back", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    // Objects earlier tests handed to gc::releaseLater go first, so the counts below are this
    // test's.
    dx::gc::Options none;
    none.candidate = [](const dx::UObject*) { return false; };
    dx::gc::collect(none);
    dx::UObject* canvas = dx::objects::newObject(dx::objects::findClass("Engine.Canvas"), dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
    dx::UObject* font = dx::objects::newObject(dx::objects::findClass("Engine.Canvas"), dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
    REQUIRE(canvas != nullptr);
    REQUIRE(font != nullptr);
    uint32_t canvasHandle = canvas->handle;
    uint32_t fontHandle = font->handle;
    dx::UObject*& field = *reinterpret_cast<dx::UObject**>(canvas->propertyAddress(dx::Name("Font")));

    // The saved copy names `font`; the canvas itself no longer does.
    dx::EditorHistory history;
    field = font;
    history.begin(nullptr, { canvas }, "test");
    history.commit();
    field = nullptr;

    dx::gc::Options options;
    options.candidate = [&](const dx::UObject* object) { return object == canvas || object == font; };
    CHECK(dx::gc::collect(options).freed == 0);
    CHECK(alive(canvas, canvasHandle));
    CHECK(alive(font, fontHandle));

    history.clear();
    CHECK(dx::gc::collect(options).freed == 2);
    CHECK_FALSE(alive(canvas, canvasHandle));
    CHECK_FALSE(alive(font, fontHandle));
}

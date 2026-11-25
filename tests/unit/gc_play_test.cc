#include "core/config.h"
#include "test_paths.h"
#include "core/gc.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "extension/windows.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

namespace {
    using dx::test::PlayFreeing;

    dx::UObject* transient()
    {
        return dx::objects::packageObject(dx::Name("Transient"));
    }

    dx::UObject*& font(dx::UObject* canvas)
    {
        return *reinterpret_cast<dx::UObject**>(canvas->propertyAddress(dx::Name("Font")));
    }

    bool alive(const dx::UObject* object, uint32_t handle)
    {
        return dx::objects::atHandle(handle) == object;
    }
}

TEST_CASE("destroyed actors are deleted once 128 are waiting, as CleanupDestroyed does", "[data][gc]")
{
    PlayFreeing freeing;
    PlayFreeing::requireReported();
    dx::test::LoadedMap map("01_NYC_UNATCOHQ");
    dx::World& world = map.world;
    dx::UClass* boxClass = dx::objects::findClass("DeusEx.BoxSmall");
    REQUIRE(boxClass != nullptr);
    dx::UObject* holder = dx::objects::newObject(dx::objects::findClass("Engine.Canvas"), transient(), dx::Name(), 0);
    dx::Vector spot = dx::actor::Location(world.player());

    auto spawnAndDestroy = [&](int count, std::vector<std::pair<dx::UObject*, uint32_t>>& made) {
        for (int i = 0; i < count; i++) {
            dx::UObject* box = world.spawn(boxClass, nullptr, dx::Name(), spot, {}, nullptr, true);
            REQUIRE(box != nullptr);
            made.emplace_back(box, box->handle);
            REQUIRE(world.destroy(box));
        }
    };

    // Below 128 they wait, still readable by whatever held them.
    std::vector<std::pair<dx::UObject*, uint32_t>> few;
    spawnAndDestroy(10, few);
    font(holder) = few.front().first;
    world.tick(1.0f / 30.0f);
    CHECK(alive(few.front().first, few.front().second));
    CHECK(font(holder) == few.front().first);

    // A level actor holding one has it cleared, as CleanupDestroyed clears the actor list.
    dx::UObject* watcher = world.spawn(boxClass, nullptr, dx::Name(), spot, {}, nullptr, true);
    REQUIRE(watcher != nullptr);
    dx::actor::Owner(watcher) = few.front().first;

    std::vector<std::pair<dx::UObject*, uint32_t>> many;
    spawnAndDestroy(130, many);
    world.tick(1.0f / 30.0f);
    CHECK(dx::actor::Owner(watcher) == nullptr);
    // Anything else is not looked through: it still points at the freed actor, as in the
    // original, until the next collection.
    CHECK(dx::gc::wasFreed(font(holder)));
    size_t gone = 0;
    for (const auto& [box, handle] : few) {
        gone += alive(box, handle) ? 0u : 1u;
    }
    for (const auto& [box, handle] : many) {
        gone += alive(box, handle) ? 0u : 1u;
    }
    CHECK(gone == few.size() + many.size());
    CHECK(world.destroyedActors.empty());
    CHECK(world.releasingActors.empty());
    // The next collection reaches the holder and sets the stale reference to None without
    // following it.
    dx::gc::Options keepAll;
    keepAll.candidate = [](const dx::UObject*) { return false; };
    dx::gc::Result collected = dx::gc::collect(keepAll);
    CHECK(collected.cleared >= 1);
    CHECK(font(holder) == nullptr);
    CHECK_FALSE(dx::gc::wasFreed(few.front().first));
    world.destroy(watcher);
    dx::gc::release({ holder });
}

TEST_CASE("destroyed actors wait while freeing during play is not allowed", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    // Without the game reporting, nothing is deleted during play.
    dx::gc::forgetLibraryReported("game");
    dx::test::LoadedMap map("01_NYC_UNATCOHQ");
    dx::World& world = map.world;
    dx::UClass* boxClass = dx::objects::findClass("DeusEx.BoxSmall");
    std::vector<std::pair<dx::UObject*, uint32_t>> made;
    for (int i = 0; i < 130; i++) {
        dx::UObject* box = world.spawn(boxClass, nullptr, dx::Name(), dx::actor::Location(world.player()), {}, nullptr, true);
        REQUIRE(box != nullptr);
        made.emplace_back(box, box->handle);
        world.destroy(box);
    }
    world.tick(1.0f / 30.0f);
    for (const auto& [box, handle] : made) {
        CHECK(alive(box, handle));
    }
}

TEST_CASE("a destroyed window is deleted at the end of the extension's tick", "[data][gc]")
{
    PlayFreeing freeing;
    PlayFreeing::requireReported();
    dx::extension::reset();
    std::string previous = dx::config::system().get("Engine.Engine", "Root").value_or("DeusEx.DeusExRootWindow");
    dx::test::atTestEnd([previous] {
        dx::config::system().set("Engine.Engine", "Root", previous);
    });
    dx::config::system().set("Engine.Engine", "Root", "Extension.RootWindow");
    dx::UObject* player = dx::objects::newObject(dx::objects::findClass("Extension.PlayerPawnExt"), transient(), dx::Name(), 0);
    dx::vm::Call init(player, dx::Name("InitRootWindow"));
    init.run();
    dx::UObject* root = dx::extension::rootWindow();
    REQUIRE(root != nullptr);
    dx::vm::Call make(root, dx::Name("NewChild"));
    make.obj(dx::objects::findClass("Extension.Window")).b(true);
    make.run();
    dx::UObject* window = *reinterpret_cast<dx::UObject**>(make.returnValue());
    REQUIRE(window != nullptr);
    uint32_t handle = window->handle;
    dx::UObject* holder = dx::objects::newObject(dx::objects::findClass("Engine.Canvas"), transient(), dx::Name(), 0);
    font(holder) = window;

    dx::vm::Call destroy(window, dx::Name("Destroy"));
    destroy.run();
    CHECK(window->destroyed);
    CHECK(alive(window, handle));
    dx::extension::tick(0.01f);
    CHECK_FALSE(alive(window, handle));
    // Freed without looking through other objects, as XWindow::SafeDestroy does.
    CHECK(dx::gc::wasFreed(font(holder)));
    dx::gc::Options keepAll;
    keepAll.candidate = [](const dx::UObject*) { return false; };
    dx::gc::collect(keepAll);
    CHECK(font(holder) == nullptr);
    dx::gc::release({ holder });
    dx::extension::reset();
}

TEST_CASE("KeepFreedObjects holds freed windows until the next collection and marks them", "[data][gc]")
{
    PlayFreeing freeing;
    PlayFreeing::requireReported();
    dx::gc::setKeepFreed(true);
    dx::test::atTestEnd([] { dx::gc::setKeepFreed(false); });
    dx::extension::reset();
    std::string previous = dx::config::system().get("Engine.Engine", "Root").value_or("DeusEx.DeusExRootWindow");
    dx::test::atTestEnd([previous] {
        dx::config::system().set("Engine.Engine", "Root", previous);
    });
    dx::config::system().set("Engine.Engine", "Root", "Extension.RootWindow");
    dx::UObject* player = dx::objects::newObject(dx::objects::findClass("Extension.PlayerPawnExt"), transient(), dx::Name(), 0);
    dx::vm::Call init(player, dx::Name("InitRootWindow"));
    init.run();
    dx::UObject* root = dx::extension::rootWindow();
    REQUIRE(root != nullptr);
    dx::vm::Call make(root, dx::Name("NewChild"));
    make.obj(dx::objects::findClass("Extension.Window")).b(true);
    make.run();
    dx::UObject* window = *reinterpret_cast<dx::UObject**>(make.returnValue());
    REQUIRE(window != nullptr);
    uint32_t handle = window->handle;
    dx::UObject* holder = dx::objects::newObject(dx::objects::findClass("Engine.Canvas"), transient(), dx::Name(), 0);
    font(holder) = window;

    dx::vm::Call destroy(window, dx::Name("Destroy"));
    destroy.run();
    dx::extension::tick(0.01f);
    CHECK(alive(window, handle));
    CHECK(window->released);
    CHECK(font(holder) == window);
    CHECK(dx::gc::keptFreedObjects() >= 1);

    dx::gc::Options keepAll;
    keepAll.candidate = [](const dx::UObject*) { return false; };
    dx::gc::collect(keepAll);
    CHECK_FALSE(alive(window, handle));
    CHECK(font(holder) == nullptr);
    dx::gc::release({ holder });
    dx::extension::reset();
}

#include "core/gc.h"
#include "test_paths.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/startup.h"
#include "engine/ai.h"
#include "engine/csg.h"
#include "engine/mesh.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace {
    // Objects made by one test. Collections are limited to them, so what other tests left
    // behind is untouched, except objects earlier tests handed to gc::releaseLater, such as
    // the windows of a reset window tree: those go first, so the counts below are this test's.
    struct Scratch {
        std::vector<dx::UObject*> made;
        std::unordered_set<const dx::UObject*> candidates;

        Scratch()
        {
            dx::gc::Options none;
            none.candidate = [](const dx::UObject*) { return false; };
            dx::gc::collect(none);
        }

        dx::UObject* package()
        {
            return dx::objects::packageObject(dx::Name("Transient"));
        }

        dx::UMesh* mesh(bool candidate = true)
        {
            auto* object = static_cast<dx::UMesh*>(dx::objects::newObject(dx::csg::nativeClass("Mesh"), package(), dx::Name(), 0));
            add(object, candidate);
            return object;
        }

        dx::UObject* canvas(bool candidate = true)
        {
            dx::UObject* object = dx::objects::newObject(dx::objects::findClass("Engine.Canvas"), package(), dx::Name(), 0);
            add(object, candidate);
            return object;
        }

        void add(dx::UObject* object, bool candidate)
        {
            REQUIRE(object != nullptr);
            made.push_back(object);
            if (candidate) {
                candidates.insert(object);
            }
        }

        dx::gc::Options options()
        {
            dx::gc::Options result;
            result.candidate = [this](const dx::UObject* object) { return candidates.count(object) != 0; };
            return result;
        }

        static bool alive(const dx::UObject* object, uint32_t handle)
        {
            return dx::objects::atHandle(handle) == object;
        }

        // Deletes whatever the test left, so later tests see none of it.
        ~Scratch()
        {
            std::vector<dx::UObject*> left;
            for (dx::UObject* object : made) {
                if (std::find(dx::objects::all().begin(), dx::objects::all().end(), object) != dx::objects::all().end()) {
                    left.push_back(object);
                }
            }
            dx::gc::release(left);
        }
    };

    dx::UObject*& font(dx::UObject* canvas)
    {
        return *reinterpret_cast<dx::UObject**>(canvas->propertyAddress(dx::Name("Font")));
    }

    size_t countNamed(dx::Name name)
    {
        const std::vector<dx::UObject*>& table = dx::objects::all();
        return static_cast<size_t>(std::count_if(table.begin(), table.end(), [&](const dx::UObject* object) { return object && object->name == name; }));
    }

    struct TestHolder final : dx::gc::Holder {
        dx::UObject* strong = nullptr;
        std::vector<const dx::UObject*> released;

        void addReferences(dx::ReferenceVisitor& references) override
        {
            references(strong);
        }

        void objectsReleased(const dx::gc::ReleasedSet& objects) override
        {
            released.insert(released.end(), objects.begin(), objects.end());
        }
    };
}

TEST_CASE("the collector frees a cycle nothing reaches", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    dx::UMesh* a = scratch.mesh();
    dx::UMesh* b = scratch.mesh();
    a->textures = { b };
    b->textures = { a };
    uint32_t aHandle = a->handle;
    uint32_t bHandle = b->handle;
    TestHolder holder;
    dx::gc::Result result = dx::gc::collect(scratch.options());
    CHECK(result.freed == 2);
    CHECK_FALSE(Scratch::alive(a, aHandle));
    CHECK_FALSE(Scratch::alive(b, bHandle));
    CHECK(std::count(holder.released.begin(), holder.released.end(), a) == 1);
}

TEST_CASE("the collector keeps what roots reach through native references and property data", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    // A root outside the candidates holds the first mesh in its property data; that mesh
    // holds the second through its C++ texture list.
    dx::UObject* root = scratch.canvas(false);
    dx::UMesh* first = scratch.mesh();
    dx::UMesh* second = scratch.mesh();
    dx::UMesh* loose = scratch.mesh();
    font(root) = first;
    first->textures = { second };
    // A holder keeps a third.
    TestHolder holder;
    dx::UMesh* held = scratch.mesh();
    holder.strong = held;
    uint32_t looseHandle = loose->handle;
    dx::gc::Result result = dx::gc::collect(scratch.options());
    CHECK(result.freed == 1);
    CHECK(result.kept == 3);
    CHECK(Scratch::alive(first, first->handle));
    CHECK(Scratch::alive(second, second->handle));
    CHECK(Scratch::alive(held, held->handle));
    CHECK_FALSE(Scratch::alive(loose, looseHandle));
    CHECK(first->textures[0] == second);
}

TEST_CASE("the collector sets references to eliminated objects to None", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    dx::UObject* root = scratch.canvas(false);
    dx::UMesh* holderMesh = scratch.mesh(false);
    dx::UMesh* doomed = scratch.mesh();
    font(root) = doomed;
    holderMesh->textures = { doomed, root };
    // An object inside the eliminated one survives elsewhere when something keeps it.
    dx::UMesh* inner = scratch.mesh();
    inner->outer = doomed;
    holderMesh->textures.push_back(inner);
    uint32_t doomedHandle = doomed->handle;
    dx::gc::Options options = scratch.options();
    options.eliminate = [&](const dx::UObject* object) { return object == doomed; };
    dx::gc::Result result = dx::gc::collect(options);
    CHECK(result.cleared == 2);
    CHECK(result.freed == 1);
    CHECK(font(root) == nullptr);
    CHECK(holderMesh->textures[0] == nullptr);
    CHECK(holderMesh->textures[1] == root);
    CHECK_FALSE(Scratch::alive(doomed, doomedHandle));
    REQUIRE(Scratch::alive(inner, inner->handle));
    CHECK(inner->outer == scratch.package());
}

TEST_CASE("a destroyed object keeps nothing", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    dx::UObject* dead = scratch.canvas(false);
    dx::objects::destroyObject(dead);
    dx::UMesh* target = scratch.mesh();
    font(dead) = target;
    uint32_t targetHandle = target->handle;
    dx::gc::Result result = dx::gc::collect(scratch.options());
    CHECK(result.freed == 1);
    CHECK(font(dead) == nullptr);
    CHECK_FALSE(Scratch::alive(target, targetHandle));
}

TEST_CASE("a destroyed object keeps its own class and state code and nothing else", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    // A class that may go, as one made at run time or one in a released map package is, with a
    // destroyed instance that is not itself a candidate, as a destroyed actor still listed in
    // its world is not.
    dx::UClass* cls = dx::objects::createSubclass(dx::objects::findClass("Engine.Canvas"), dx::Name("GcTestDestroyedCanvas"));
    REQUIRE(cls->defaultObject != nullptr);
    scratch.add(cls, true);
    scratch.add(cls->defaultObject, true);
    dx::UObject* instance = dx::objects::newObject(cls, scratch.package(), dx::Name(), 0);
    scratch.add(instance, false);
    dx::StateFrame& frame = dx::vm::stateFrame(instance);
    REQUIRE(frame.node == cls);
    dx::objects::destroyObject(instance);
    dx::UMesh* target = scratch.mesh();
    font(instance) = target;
    uint32_t classHandle = cls->handle;
    uint32_t targetHandle = target->handle;

    dx::gc::collect(scratch.options());
    CHECK(instance->cls == cls);
    CHECK(frame.node == cls);
    CHECK(Scratch::alive(cls, classHandle));
    CHECK(font(instance) == nullptr);
    CHECK_FALSE(Scratch::alive(target, targetHandle));
}

TEST_CASE("a destroyed object is not kept for being inside a kept outer", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    // As a FlagBase is kept, and a flag deleted from it stays inside it unlinked.
    dx::UMesh* outer = scratch.mesh();
    dx::UMesh* inner = scratch.mesh();
    dx::UMesh* deleted = scratch.mesh();
    inner->outer = outer;
    deleted->outer = outer;
    dx::objects::destroyObject(deleted);
    TestHolder holder;
    holder.strong = outer;
    uint32_t deletedHandle = deleted->handle;
    dx::gc::Options options = scratch.options();
    options.keepInners = true;
    dx::gc::Result result = dx::gc::collect(options);
    CHECK(result.freed == 1);
    CHECK(Scratch::alive(inner, inner->handle));
    CHECK_FALSE(Scratch::alive(deleted, deletedHandle));
}

TEST_CASE("a kept object whose outer and chosen new outer both go moves to the transient package", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    dx::UMesh* outer = scratch.mesh();
    dx::UMesh* replacement = scratch.mesh();
    dx::UMesh* inner = scratch.mesh();
    inner->outer = outer;
    TestHolder holder;
    holder.strong = inner;
    uint32_t outerHandle = outer->handle;
    uint32_t replacementHandle = replacement->handle;
    dx::gc::Options options = scratch.options();
    // Eliminated, or the inner object would keep it.
    options.eliminate = [&](const dx::UObject* object) { return object == outer; };
    bool asked = false;
    options.adoptOuter = [&](dx::UObject* given) -> dx::UObject* {
        asked = given == outer;
        return replacement;
    };
    dx::gc::Result result = dx::gc::collect(options);
    CHECK(result.freed == 2);
    CHECK(asked);
    CHECK_FALSE(Scratch::alive(outer, outerHandle));
    CHECK_FALSE(Scratch::alive(replacement, replacementHandle));
    REQUIRE(Scratch::alive(inner, inner->handle));
    CHECK(inner->outer == scratch.package());
}

TEST_CASE("a function deleted with its class leaves its native index without one", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    // An index no native or loaded function uses.
    constexpr uint16_t index = 4090;
    REQUIRE(dx::vm::indexedFunction(index) == nullptr);
    auto* function = new dx::UFunction();
    function->cls = dx::objects::intrinsicClass(dx::Name("Function"));
    function->outer = dx::objects::packageObject(dx::Name("Transient"));
    function->name = dx::Name("GcTestIndexedNative");
    function->nativeIndex = index;
    function->functionFlags = dx::FUNC_Native;
    dx::objects::registerObject(function);
    dx::vm::bindNative(function);
    CHECK(dx::vm::indexedFunction(index) == function);

    dx::gc::release({ function });
    CHECK(dx::vm::indexedFunction(index) == nullptr);

    // A call by that index now finds no function, and skips its arguments as for any index
    // nothing is bound to.
    const uint8_t code[] = { static_cast<uint8_t>(0x60 | (index >> 8)), static_cast<uint8_t>(index & 0xff), dx::vm::EX_EndFunctionParms, dx::vm::EX_Nothing };
    dx::Frame frame;
    frame.object = dx::objects::packageObject(dx::Name("Transient"));
    frame.code = code;
    frame.step(frame.object, nullptr);
    CHECK(frame.code == code + 3);
}

TEST_CASE("the default candidate rule makes no package", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    // Outside any package and from no file, so neither its outer nor its linker settles it.
    dx::Name name("GcTestOuterless");
    dx::UMesh* loose = scratch.mesh();
    loose->name = name;
    loose->outer = nullptr;
    REQUIRE(countNamed(name) == 1);
    CHECK(dx::gc::isDefaultCandidate(loose));
    CHECK(countNamed(name) == 1);
    CHECK(dx::objects::findPackageObject(name) == nullptr);
}

TEST_CASE("releasing objects clears every reference to them first", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    dx::UObject* root = scratch.canvas(false);
    dx::UMesh* other = scratch.mesh(false);
    dx::UMesh* gone = scratch.mesh(false);
    font(root) = gone;
    other->textures = { gone };
    TestHolder holder;
    holder.strong = gone;
    uint32_t goneHandle = gone->handle;
    dx::gc::Result result = dx::gc::release({ gone });
    CHECK(result.freed == 1);
    CHECK(result.cleared == 3);
    CHECK(font(root) == nullptr);
    CHECK(other->textures[0] == nullptr);
    CHECK(holder.strong == nullptr);
    CHECK_FALSE(Scratch::alive(gone, goneHandle));
}

TEST_CASE("with collection off, neither a collection nor a release deletes anything", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    dx::UObject* root = scratch.canvas(false);
    dx::UMesh* loose = scratch.mesh();
    dx::UMesh* named = scratch.mesh(false);
    font(root) = named;
    uint32_t looseHandle = loose->handle;
    uint32_t namedHandle = named->handle;

    dx::gc::setEnabled(false);
    dx::gc::Result collected = dx::gc::collect(scratch.options());
    dx::gc::Result released = dx::gc::release({ named });
    dx::gc::setEnabled(true);

    CHECK(collected.freed == 0);
    CHECK(released.freed == 0);
    CHECK(released.cleared == 0);
    CHECK(Scratch::alive(loose, looseHandle));
    CHECK(Scratch::alive(named, namedHandle));
    CHECK(font(root) == named);
}

TEST_CASE("objects handed over for later go at the next collection", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    dx::UObject* root = scratch.canvas(false);
    dx::UMesh* window = scratch.mesh(false);
    dx::objects::destroyObject(window);
    font(root) = window;
    uint32_t windowHandle = window->handle;
    dx::gc::releaseLater({ window });
    dx::gc::Result result = dx::gc::collect(scratch.options());
    CHECK(result.freed == 1);
    CHECK(font(root) == nullptr);
    CHECK_FALSE(Scratch::alive(window, windowHandle));
}

TEST_CASE("a class goes with its default object and instances", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    dx::UClass* cls = dx::objects::createSubclass(dx::objects::findClass("Engine.Canvas"), dx::Name("GcTestCanvas"));
    REQUIRE(cls->defaultObject != nullptr);
    dx::UObject* defaults = cls->defaultObject;
    dx::UObject* instance = dx::objects::newObject(cls, scratch.package(), dx::Name(), 0);
    scratch.add(cls, true);
    scratch.add(defaults, true);
    scratch.add(instance, true);
    uint32_t classHandle = cls->handle;
    uint32_t defaultsHandle = defaults->handle;

    // An instance something holds keeps its class.
    TestHolder holder;
    holder.strong = instance;
    dx::gc::collect(scratch.options());
    CHECK(Scratch::alive(cls, classHandle));
    CHECK(Scratch::alive(defaults, defaultsHandle));

    holder.strong = nullptr;
    dx::gc::Result result = dx::gc::collect(scratch.options());
    CHECK(result.freed == 3);
    CHECK_FALSE(Scratch::alive(cls, classHandle));
    CHECK_FALSE(Scratch::alive(defaults, defaultsHandle));
}

TEST_CASE("objects of a type that has not said it reports its references are listed", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());

    struct Unreported : dx::UObject {
    };

    auto* object = new Unreported();
    object->cls = dx::objects::objectClass();
    object->outer = dx::objects::packageObject(dx::Name("Transient"));
    object->name = dx::Name("GcTestUnreported");
    dx::objects::registerObject(object);
    CHECK_FALSE(dx::gc::reportsReferences(object));
    std::vector<std::string> types = dx::gc::unreportedTypes();
    CHECK(std::find(types.begin(), types.end(), std::string(typeid(Unreported).name())) != types.end());
    CHECK_FALSE(dx::gc::everythingReported());
    CHECK(dx::gc::reportsReferences(dx::objects::objectClass()));
    dx::gc::release({ object });
}

// A full collection with no candidate rule would free whatever other tests in this process left
// unreached, including objects of types that do not report their references yet, so it is not
// run here. The rule itself is checked, and a collection limited to this test's objects shows
// that the rule keeps the roots it names.
TEST_CASE("the default candidate rule keeps native objects and loaded packages", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    dx::UClass* intrinsic = dx::csg::nativeClass("Mesh");
    dx::UClass* actorClass = dx::objects::findClass("Engine.Actor");
    dx::UObject* engine = dx::objects::packageObject(dx::Name("Engine"));
    dx::UObject* transient = scratch.package();
    REQUIRE(actorClass != nullptr);
    CHECK_FALSE(dx::gc::isDefaultCandidate(intrinsic));
    CHECK_FALSE(dx::gc::isDefaultCandidate(actorClass));
    CHECK_FALSE(dx::gc::isDefaultCandidate(engine));
    CHECK_FALSE(dx::gc::isDefaultCandidate(transient));

    dx::UMesh* loose = scratch.mesh(false);
    dx::UMesh* reached = scratch.mesh(false);
    CHECK(dx::gc::isDefaultCandidate(loose));
    TestHolder holder;
    holder.strong = reached;
    uint32_t looseHandle = loose->handle;
    std::unordered_set<const dx::UObject*> chosen = { intrinsic, actorClass, engine, transient, loose, reached };
    dx::gc::Options options;
    options.candidate = [&](const dx::UObject* object) { return chosen.count(object) && dx::gc::isDefaultCandidate(object); };
    dx::gc::Result result = dx::gc::collect(options);
    CHECK(result.freed == 1);
    CHECK_FALSE(Scratch::alive(loose, looseHandle));
    CHECK(Scratch::alive(reached, reached->handle));
    CHECK(Scratch::alive(actorClass, actorClass->handle));
    CHECK(Scratch::alive(intrinsic, intrinsic->handle));
    CHECK(Scratch::alive(engine, engine->handle));
}

TEST_CASE("unlinked AI events wait for the next tick while a collection runs", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    auto* manager = static_cast<dx::UEventManager*>(dx::objects::newObject(dx::objects::intrinsicClass(dx::Name("EventManager")), scratch.package(), dx::Name(), 0));
    scratch.add(manager, false);
    auto* event = static_cast<dx::UAIEvent*>(dx::objects::newObject(dx::objects::intrinsicClass(dx::Name("AISenderEvent")), manager, dx::Name(), 0));
    scratch.add(event, false);
    dx::objects::destroyObject(event);
    manager->unlinked.push_back(event);
    uint32_t eventHandle = event->handle;

    struct DuringCollection final : dx::gc::Holder {
        dx::UEventManager* manager = nullptr;

        void addReferences(dx::ReferenceVisitor&) override
        {
            manager->releaseUnlinked();
        }
    } during;

    during.manager = manager;
    dx::gc::collect(scratch.options());
    CHECK(manager->unlinked.size() == 1);
    CHECK(Scratch::alive(event, eventHandle));

    manager->releaseUnlinked();
    CHECK(manager->unlinked.empty());
    CHECK_FALSE(Scratch::alive(event, eventHandle));
}

TEST_CASE("the reported answer follows objects as they are registered and released", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::gc::libraryReported("render");
    dx::gc::libraryReported("game");
    dx::test::atTestEnd([] { dx::gc::forgetLibraryReported("game"); });
    if (!dx::gc::everythingReported()) {
        SKIP("an earlier test left objects that do not report their references");
    }

    struct Unreported : dx::UObject {
    };

    auto* object = new Unreported();
    object->cls = dx::objects::objectClass();
    object->outer = dx::objects::packageObject(dx::Name("Transient"));
    object->name = dx::Name("GcTestCacheUnreported");
    dx::objects::registerObject(object);
    CHECK_FALSE(dx::gc::everythingReported());
    CHECK_FALSE(dx::gc::unreportedTypes().empty());
    dx::gc::release({ object });
    CHECK(dx::gc::everythingReported());
    CHECK(dx::gc::unreportedTypes().empty());

    // A type said to report later counts from then on.
    struct LateReported : dx::UObject {
    };

    auto* late = new LateReported();
    late->cls = dx::objects::objectClass();
    late->outer = dx::objects::packageObject(dx::Name("Transient"));
    late->name = dx::Name("GcTestCacheLate");
    dx::objects::registerObject(late);
    CHECK_FALSE(dx::gc::everythingReported());
    dx::gc::reportsReferences<LateReported>();
    CHECK(dx::gc::everythingReported());
    dx::gc::release({ late });
}

TEST_CASE("the collector counts its collections and releases and what they freed", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    Scratch scratch;
    dx::gc::Stats before = dx::gc::stats();
    scratch.mesh();
    scratch.mesh();
    dx::gc::Result collected = dx::gc::collect(scratch.options());
    REQUIRE(collected.freed == 2);
    dx::gc::Stats afterCollect = dx::gc::stats();
    CHECK(afterCollect.collections == before.collections + 1);
    CHECK(afterCollect.collectedObjects == before.collectedObjects + 2);
    CHECK(afterCollect.lastCollectedObjects == 2);
    CHECK(afterCollect.lastCollectMilliseconds >= 0);
    CHECK(afterCollect.releases == before.releases);
    dx::UMesh* released = scratch.mesh();
    dx::gc::release({ released });
    dx::gc::Stats afterRelease = dx::gc::stats();
    CHECK(afterRelease.releases == afterCollect.releases + 1);
    CHECK(afterRelease.releasedObjects == afterCollect.releasedObjects + 1);
    CHECK(afterRelease.collections == afterCollect.collections);
    // Nothing to release is not counted.
    dx::gc::release({});
    CHECK(dx::gc::stats().releases == afterRelease.releases);
}
// Level changes with every library reporting its references, so the collection after each one
// uses the full rule the game uses: anything unreached that is not native and not part of a
// loaded package is deleted.
#include "core/config.h"
#include "core/gc.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/gamepad_layout.h"
#include "deusex/script.h"
#include "editor/log_capture.h"
#include "engine/actor.h"
#include "engine/resources.h"
#include "engine/savegame.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "fixtures.h"
#include "layout_row.h"

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
    using dx::test::PlayFreeing;

    constexpr float TickSeconds = 1.0f / 30.0f;
    constexpr const char* Island = "01_NYC_UNATCOIsland";
    constexpr const char* Headquarters = "01_NYC_UNATCOHQ";

    dx::UObject*& objectProperty(dx::UObject* object, const char* name, int32_t index = 0)
    {
        return *reinterpret_cast<dx::UObject**>(object->propertyAddress(dx::Name(name), index));
    }

    void tick(dx::World& world, int ticks)
    {
        for (int i = 0; i < ticks; i++) {
            world.tick(TickSeconds);
        }
    }

    // Keeps one object, as a travelling instance keeps its class.
    struct Held final : dx::gc::Holder {
        dx::UObject* object = nullptr;

        void addReferences(dx::ReferenceVisitor& references) override
        {
            references(object);
        }
    };

    // The lines logged since the capture started.
    std::vector<std::string> capturedLines(dx::editor::LogCapture& capture)
    {
        capture.stop();
        return capture.lines();
    }

    // Opens a screen with the Controller Layout row and presses its button. True when the layout
    // screen is then on top.
    bool openControllerLayout()
    {
        dx::UObject* button = dx::test::openLayoutRow();
        REQUIRE(button != nullptr);
        return dx::padlayout::buttonPressed(button) && dx::script::isA(dx::extension::topModal(), "MenuScreenControllerLayout");
    }

    void useDeusExRootWindow()
    {
        std::string previous = dx::config::system().get("Engine.Engine", "Root").value_or("DeusEx.DeusExRootWindow");
        dx::test::atTestEnd([previous] {
            dx::config::system().set("Engine.Engine", "Root", previous);
        });
        dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
    }
}

// padlayout::install makes the screen class once at start-up and finds it by name when the button
// is pressed; nothing in script data names it.
TEST_CASE("the Controller Layout screen survives a level change and its button still opens it", "[data][gc]")
{
    PlayFreeing freeing;
    PlayFreeing::requireReported();
    useDeusExRootWindow();
    dx::padlayout::install();
    REQUIRE(dx::objects::findClass("DeusEx.MenuScreenControllerLayout") != nullptr);
    dx::test::SaveRoot saves("gc-layout");
    dx::savegame::deleteCurrent();
    dx::test::PackageGuard island(Island);
    dx::test::PackageGuard headquarters(Headquarters);
    dx::test::LevelChanges levels;
    REQUIRE(levels.open(Island, false));
    REQUIRE(levels.open(Headquarters, true));
    REQUIRE(dx::gc::everythingReported());
    CHECK(dx::objects::findClass("DeusEx.MenuScreenControllerLayout") != nullptr);
    CHECK(openControllerLayout());
}

// A class saved inside a map package goes with the package unless something still reaches it, as
// a travelling instance of it does. Its default object has the package as its outer too and is
// an Actor of that class, but it is not one of the level's actors. No stock map embeds a class,
// so the test makes one and moves it into the level's package.
TEST_CASE("a class of a level package that outlives its level keeps its default object", "[data][gc]")
{
    PlayFreeing freeing;
    PlayFreeing::requireReported();
    dx::test::SaveRoot saves("gc-level-class");
    dx::savegame::deleteCurrent();
    dx::test::PackageGuard island(Island);
    dx::test::PackageGuard headquarters(Headquarters);
    dx::test::LevelChanges levels;
    REQUIRE(levels.open(Headquarters, false));
    dx::UObject* package = levels.world->level->outermost();
    dx::UClass* boxClass = dx::objects::findClass("DeusEx.BoxSmall");
    REQUIRE(boxClass != nullptr);
    dx::UClass* scratch = dx::objects::createSubclass(boxClass, dx::Name("GcLevelTestBox"));
    REQUIRE(scratch->defaultObject != nullptr);
    scratch->outer = package;
    scratch->defaultObject->outer = package;
    dx::objects::syncObjectProperties(scratch);
    dx::objects::syncObjectProperties(scratch->defaultObject);
    uint32_t defaultsHandle = scratch->defaultObject->handle;
    Held held;
    held.object = scratch;

    REQUIRE(levels.open(Island, true));
    REQUIRE(dx::gc::everythingReported());
    REQUIRE(dx::objects::atHandle(scratch->handle) == scratch);
    REQUIRE(scratch->defaultObject != nullptr);
    CHECK(dx::objects::atHandle(defaultsHandle) == scratch->defaultObject);
    CHECK(scratch->outer != package);
    dx::UObject* player = levels.world->player();
    REQUIRE(player != nullptr);
    dx::UObject* box = levels.world->spawn(scratch, nullptr, dx::Name(), dx::actor::Location(player), {}, nullptr, true);
    REQUIRE(box != nullptr);
    tick(*levels.world, 2);
    levels.world->destroy(box);
    tick(*levels.world, 1);

    held.object = nullptr;
    levels.close();
    dx::gc::release({ scratch->defaultObject, scratch });
}

// The production rule over a round trip and a quickload: what the player carries goes with them,
// classes made at run time stay, every handle still names its object, and play and saving go on.
TEST_CASE("a round trip and a quickload with every library reporting keep the player's things", "[data][gc]")
{
    PlayFreeing freeing;
    PlayFreeing::requireReported();
    useDeusExRootWindow();
    dx::padlayout::install();
    dx::test::SaveRoot saves("gc-level-round-trip");
    dx::savegame::deleteCurrent();
    dx::test::PackageGuard island(Island);
    dx::test::PackageGuard headquarters(Headquarters);

    // A menu choice made the way the game makes its own: a subclass of a stock choice, named in
    // a stock screen's defaults.
    dx::UClass* choiceParent = dx::objects::findClass("DeusEx.MenuUIChoiceEnum");
    dx::UClass* displayScreen = dx::objects::findClass("DeusEx.MenuScreenDisplay");
    REQUIRE(choiceParent != nullptr);
    REQUIRE(displayScreen != nullptr);
    dx::UClass* choice = dx::objects::createSubclass(choiceParent, dx::Name("MenuChoice_GcLevelTest"));
    int32_t lastChoice = displayScreen->findProperty(dx::Name("choices"))->arrayDim - 1;
    dx::UObject*& choiceSlot = objectProperty(displayScreen->defaultObject, "choices", lastChoice);
    REQUIRE(choiceSlot == nullptr);
    choiceSlot = choice;
    dx::test::atTestEnd([displayScreen, lastChoice] {
        objectProperty(displayScreen->defaultObject, "choices", lastChoice) = nullptr;
    });

    // Every class made at run time so far, the menus among them.
    struct Made {
        dx::UObject* cls;
        uint32_t handle;
        std::string name;
    };

    std::vector<Made> madeClasses;
    for (dx::UObject* object : dx::objects::all()) {
        if (object && dx::cast<dx::UClass>(object) && !object->linker && !(object->flags & dx::RF_Native)) {
            madeClasses.push_back({ object, object->handle, object->name.str() });
        }
    }
    REQUIRE(madeClasses.size() >= 2);

    dx::editor::LogCapture capture;
    capture.start();
    dx::test::LevelChanges levels;
    REQUIRE(levels.open(Island, false));
    tick(*levels.world, 30);
    dx::UObject* player = levels.world->player();
    REQUIRE(player != nullptr);
    {
        dx::vm::Call note(player, dx::Name("AddNote"));
        note.s("a note that travels").b(true).b(false);
        note.run();
        dx::vm::Call goal(player, dx::Name("AddGoal"));
        goal.n(dx::Name("GcLevelTestGoal")).b(true);
        goal.run();
        dx::UObject* made = *reinterpret_cast<dx::UObject**>(goal.returnValue());
        REQUIRE(made != nullptr);
        *reinterpret_cast<std::string*>(made->propertyAddress(dx::Name("text"))) = "a goal that travels";
        dx::UObject* keyRing = objectProperty(player, "KeyRing");
        REQUIRE(keyRing != nullptr);
        dx::vm::Call key(keyRing, dx::Name("GiveKey"));
        key.n(dx::Name("GcLevelTestKey")).s("a key that travels");
        key.run();
    }

    auto checkCarried = [&](const std::string& where) {
        INFO("after " << where);
        dx::UObject* carrier = levels.world->player();
        REQUIRE(carrier != nullptr);
        bool noted = false;
        int guard = 0;
        for (dx::UObject* note = objectProperty(carrier, "FirstNote"); note && guard < 256; note = objectProperty(note, "next"), guard++) {
            noted = noted || *reinterpret_cast<std::string*>(note->propertyAddress(dx::Name("text"))) == "a note that travels";
        }
        CHECK(noted);
        dx::vm::Call find(carrier, dx::Name("FindGoal"));
        find.n(dx::Name("GcLevelTestGoal"));
        find.run();
        dx::UObject* goal = *reinterpret_cast<dx::UObject**>(find.returnValue());
        REQUIRE(goal != nullptr);
        CHECK(*reinterpret_cast<std::string*>(goal->propertyAddress(dx::Name("text"))) == "a goal that travels");
        dx::UObject* keyRing = objectProperty(carrier, "KeyRing");
        REQUIRE(keyRing != nullptr);
        dx::vm::Call has(keyRing, dx::Name("HasKey"));
        has.n(dx::Name("GcLevelTestKey"));
        has.run();
        CHECK(*reinterpret_cast<uint32_t*>(has.returnValue()) != 0);
        for (const Made& made : madeClasses) {
            INFO(made.name);
            CHECK(dx::objects::atHandle(made.handle) == made.cls);
        }
        CHECK(objectProperty(displayScreen->defaultObject, "choices", lastChoice) == choice);
        CHECK(openControllerLayout());
        dx::extension::reset();
    };
    auto playAndSave = [&](int32_t slot) {
        tick(*levels.world, 30);
        REQUIRE(dx::savegame::saveGame(*levels.world, slot, "gc level test"));
        tick(*levels.world, 2);
    };

    playAndSave(1);
    REQUIRE(levels.open(Headquarters, true));
    REQUIRE(dx::gc::everythingReported());
    CHECK(levels.released.freed > 1000);
    checkCarried(Headquarters);
    playAndSave(2);
    REQUIRE(levels.open(Island, true));
    CHECK(levels.released.freed > 1000);
    checkCarried(Island);
    playAndSave(dx::savegame::QuickSaveIndex);
    REQUIRE(levels.open(Headquarters, true));
    REQUIRE(levels.load(dx::savegame::QuickSaveIndex));
    CHECK(levels.released.freed > 1000);
    checkCarried("the quickload");
    playAndSave(3);
    levels.close();

    for (const std::string& line : capturedLines(capture)) {
        INFO(line);
        CHECK(line.find("names a deleted object") == std::string::npos);
    }
}

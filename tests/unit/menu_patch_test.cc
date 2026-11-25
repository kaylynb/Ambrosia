#include "core/objects.h"
#include "test_paths.h"
#include "deusex/deusex.h"
#include "deusex/menu_layout.h"
#include "deusex/startup.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

TEST_CASE("a menu patch with a missing class or property is not ready", "[gamepad]")
{
    dx::deusex::MenuPatch patch("a test patch");
    CHECK(patch.ready());
    CHECK(patch.field<int32_t>(nullptr, "ClientHeight") == nullptr);
    CHECK(patch.property(nullptr, "choices") == nullptr);
    CHECK_FALSE(patch.ready());
    // Logged once; still not ready.
    CHECK_FALSE(patch.ready());
    // Nothing to patch is skipped, not dereferenced.
    dx::deusex::MenuArt none(nullptr, "no menu");
    CHECK_FALSE(none.valid());
    none.repeat(10, 0, 10);
    CHECK_FALSE(none.apply());
    dx::deusex::growMenuBackground(nullptr, 10, 20);
    dx::deusex::insertMenuButton(nullptr, 4, "Test", nullptr);
    dx::deusex::removeMenuButton(nullptr, 0);
    dx::deusex::removeOptionRows(nullptr, { 1 });
}

namespace {
    int32_t& intField(dx::UObject* object, const char* name, int index = 0)
    {
        return *reinterpret_cast<int32_t*>(object->propertyAddress(dx::Name(name), index));
    }
}

TEST_CASE("removing a menu button moves the ones below up and shortens the menu", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    REQUIRE(dx::objects::loadPackage(dx::Name("DeusEx")) != nullptr);
    dx::UClass* settings = dx::objects::findClass("DeusEx.MenuSettings");
    REQUIRE(settings != nullptr);
    dx::UClass* copy = dx::objects::createSubclass(settings, dx::Name("MenuSettingsRemoveTest"));
    dx::UObject* menu = copy->defaultObject;
    auto name = [&](int i) { return *reinterpret_cast<std::string*>(menu->propertyAddress(dx::Name("ButtonNames"), i)); };
    dx::UProperty* buttons = copy->findProperty(dx::Name("buttonDefaults"));
    size_t yOffset = buttons->structType->findProperty(dx::Name("Y"))->offset;
    auto top = [&](int i) { return *reinterpret_cast<int32_t*>(menu->propertyAddress(dx::Name("buttonDefaults"), i) + yOffset); };
    std::vector<std::string> before;
    std::vector<int32_t> tops;
    for (int i = 0; i < 10; i++) {
        before.push_back(name(i));
        tops.push_back(top(i));
    }
    int32_t height = intField(menu, "ClientHeight");
    int32_t gap = tops[1] - tops[0];
    dx::deusex::removeMenuButton(menu, 0);
    for (int i = 0; i < 9; i++) {
        CHECK(name(i) == before[static_cast<size_t>(i) + 1]);
        CHECK(top(i) == tops[static_cast<size_t>(i) + 1] - gap);
    }
    CHECK(name(9).empty());
    CHECK(name(0) == before[1]);
    CHECK(top(0) == tops[0]);
    CHECK(intField(menu, "ClientHeight") == height - gap);
}

TEST_CASE("menu buttons swap places and are found by what they open", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    REQUIRE(dx::objects::loadPackage(dx::Name("DeusEx")) != nullptr);
    dx::UClass* settings = dx::objects::findClass("DeusEx.MenuSettings");
    REQUIRE(settings != nullptr);
    dx::UObject* menu = dx::objects::createSubclass(settings, dx::Name("MenuSettingsSwapTest"))->defaultObject;
    dx::UClass* keys = dx::objects::findClass("DeusEx.MenuScreenCustomizeKeys");
    dx::UClass* controls = dx::objects::findClass("DeusEx.MenuScreenControls");
    auto name = [&](int i) { return *reinterpret_cast<std::string*>(menu->propertyAddress(dx::Name("ButtonNames"), i)); };
    dx::UProperty* buttons = menu->cls->findProperty(dx::Name("buttonDefaults"));
    size_t yOffset = buttons->structType->findProperty(dx::Name("Y"))->offset;
    auto top = [&](int i) { return *reinterpret_cast<int32_t*>(menu->propertyAddress(dx::Name("buttonDefaults"), i) + yOffset); };
    REQUIRE(dx::deusex::findMenuButton(menu, keys) == 0);
    REQUIRE(dx::deusex::findMenuButton(menu, controls) == 1);
    std::string first = name(0);
    std::string second = name(1);
    int32_t y0 = top(0);
    int32_t y1 = top(1);
    dx::deusex::swapMenuButtons(menu, 0, 1);
    CHECK(dx::deusex::findMenuButton(menu, keys) == 1);
    CHECK(dx::deusex::findMenuButton(menu, controls) == 0);
    CHECK(name(0) == second);
    CHECK(name(1) == first);
    // Each keeps the other's place.
    CHECK(top(0) == y0);
    CHECK(top(1) == y1);
    CHECK(dx::deusex::findMenuButton(menu, nullptr) == -1);
    CHECK(dx::deusex::findMenuButton(menu, dx::objects::findClass("DeusEx.MenuScreenControllerLayout")) == -1);
}

TEST_CASE("removing options rows moves the choices below up and shortens the screen", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    REQUIRE(dx::objects::loadPackage(dx::Name("DeusEx")) != nullptr);
    dx::UClass* display = dx::objects::findClass("DeusEx.MenuScreenDisplay");
    REQUIRE(display != nullptr);
    dx::UClass* copy = dx::objects::createSubclass(display, dx::Name("MenuScreenDisplayRemoveTest"));
    dx::UObject* screen = copy->defaultObject;
    auto choice = [&](int i) { return *reinterpret_cast<dx::UObject**>(screen->propertyAddress(dx::Name("choices"), i)); };
    int count = copy->findProperty(dx::Name("choices"))->arrayDim;
    std::vector<dx::UObject*> before;
    for (int i = 0; i < count; i++) {
        before.push_back(choice(i));
    }
    REQUIRE(before[3] != nullptr);
    int32_t height = intField(screen, "ClientHeight");
    int32_t help = intField(screen, "helpPosY");
    int32_t gap = intField(screen, "choiceVerticalGap");
    // In any order, and a row given twice only once.
    dx::deusex::removeOptionRows(screen, { 1, 3, 3 });
    CHECK(choice(0) == before[0]);
    CHECK(choice(1) == before[2]);
    for (int i = 2; i < count - 2; i++) {
        CHECK(choice(i) == before[static_cast<size_t>(i) + 2]);
    }
    CHECK(choice(count - 1) == nullptr);
    CHECK(choice(count - 2) == nullptr);
    CHECK(intField(screen, "ClientHeight") == height - 2 * gap);
    CHECK(intField(screen, "helpPosY") == help - 2 * gap);
    // A row with no choice in it is left alone.
    dx::deusex::removeOptionRows(screen, { count - 1 });
    CHECK(intField(screen, "ClientHeight") == height - 2 * gap);
}

TEST_CASE("menu patches check the menus they change", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    REQUIRE(dx::objects::loadPackage(dx::Name("DeusEx")) != nullptr);
    dx::deusex::MenuPatch patch("a test patch");
    CHECK(patch.findClass("DeusEx.NoSuchMenuScreen") == nullptr);
    CHECK_FALSE(patch.ready());

    dx::UClass* options = dx::objects::findClass("DeusEx.MenuScreenOptions");
    REQUIRE(options != nullptr);
    dx::UClass* copy = dx::objects::createSubclass(options, dx::Name("MenuScreenSpliceTest"));
    dx::UObject* defaults = copy->defaultObject;
    int32_t rows = *reinterpret_cast<int32_t*>(defaults->propertyAddress(dx::Name("textureRows")));
    REQUIRE(rows > 0);
    // Taking out rows past the bottom of the art keeps all of it.
    {
        dx::deusex::MenuArt art(defaults, "a test splice");
        REQUIRE(art.valid());
        art.remove(100000, 100000);
        CHECK(art.apply());
    }
    int32_t spliced = *reinterpret_cast<int32_t*>(defaults->propertyAddress(dx::Name("textureRows")));
    CHECK(spliced > 0);
    CHECK(spliced <= rows);
    CHECK(*reinterpret_cast<dx::UObject**>(defaults->propertyAddress(dx::Name("clientTextures"))) != nullptr);
    // A band starting above the top repeats from the first row, within the tiles the menu has.
    {
        dx::deusex::MenuArt art(defaults, "a test splice");
        art.repeat(40, -40, 40);
        CHECK(art.apply());
    }
    CHECK(*reinterpret_cast<int32_t*>(defaults->propertyAddress(dx::Name("textureRows"))) >= spliced);
    // An options screen has no buttons to insert among.
    if (!copy->findProperty(dx::Name("buttonDefaults"))) {
        int32_t height = *reinterpret_cast<int32_t*>(defaults->propertyAddress(dx::Name("ClientHeight")));
        dx::deusex::insertMenuButton(defaults, 4, "Test", nullptr);
        CHECK(*reinterpret_cast<int32_t*>(defaults->propertyAddress(dx::Name("ClientHeight"))) == height);
    }
}

TEST_CASE("an options screen's layout is where its original art has the slots", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    REQUIRE(dx::objects::loadPackage(dx::Name("DeusEx")) != nullptr);
    // A copy, so another test's changes to the Sound screen's defaults do not count.
    dx::UClass* sound = dx::objects::findClass("DeusEx.MenuScreenSound");
    REQUIRE(sound != nullptr);
    std::optional<dx::deusex::MenuLayout> layout = dx::deusex::optionsLayout(dx::objects::createSubclass(sound, dx::Name("MenuScreenLayoutTest")));
    REQUIRE(layout);
    CHECK(layout->width == 537);
    CHECK(layout->height == 408);
    // Measured from MenuSoundBackground: four sliders,
    // each a label, a track and a value; five choices of a label and a value; the help box.
    REQUIRE(layout->slots.size() == 4 * 3 + 5 * 2 + 1);
    using Kind = dx::deusex::MenuSlot::Kind;
    auto is = [&](size_t i, Kind kind, int x, int y, int w, int h) {
        const dx::deusex::MenuSlot& slot = layout->slots[i];
        CHECK(slot.kind == kind);
        CHECK(slot.x == x);
        CHECK(slot.y == y);
        CHECK(slot.width == w);
        CHECK(slot.height == h);
    };
    is(0, Kind::Button, 6, 26, 245, 21);
    is(1, Kind::Track, 276, 25, 179, 23);
    is(2, Kind::Field, 460, 26, 62, 21);
    is(12, Kind::Button, 6, 170, 245, 21);
    is(13, Kind::Field, 276, 170, 85, 21);
    is(22, Kind::TextBox, 6, 353, 516, 29);
    // The hash names the layout: the same layout the same, a moved slot not.
    std::string hash = dx::deusex::layoutHash(*layout);
    CHECK(hash.size() == 8);
    CHECK(dx::deusex::layoutHash(*layout) == hash);
    layout->slots[3].y += 1;
    CHECK(dx::deusex::layoutHash(*layout) != hash);
    CHECK(dx::deusex::layoutJson(*layout).find("\"kind\": \"textbox\", \"x\": 6, \"y\": 353, \"w\": 516, \"h\": 29") != std::string::npos);
}

TEST_CASE("menu art edits build on each other", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    REQUIRE(dx::objects::loadPackage(dx::Name("DeusEx")) != nullptr);
    dx::UClass* sound = dx::objects::findClass("DeusEx.MenuScreenSound");
    REQUIRE(sound != nullptr);
    dx::UObject* defaults = dx::objects::createSubclass(sound, dx::Name("MenuScreenArtTest"))->defaultObject;
    dx::deusex::MenuArt art(defaults, "a test of menu art");
    REQUIRE(art.valid());
    int height = art.height();
    REQUIRE(height > 100);
    CHECK(art.width() > 0);
    // Repeating a band and taking out the same rows again leaves the height as it was.
    art.repeat(50, 10, 30);
    CHECK(art.height() == height + 20);
    art.remove(50, 70);
    CHECK(art.height() == height);
    art.cutTo(100);
    CHECK(art.height() == 100);
    CHECK(art.apply());
    CHECK(intField(defaults, "textureRows") == 1);
}

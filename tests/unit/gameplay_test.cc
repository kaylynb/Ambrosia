#include "core/config.h"
#include "test_paths.h"
#include "core/objects.h"
#include "core/settings.h"
#include "core/vm.h"
#include "deusex/deusex.h"
#include "deusex/gamepad_menus.h"
#include "deusex/pad_inventory.h"
#include "deusex/prompts.h"
#include "deusex/script.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/console.h"
#include "engine/input.h"
#include "engine/props_engine.h"
#include "engine/props_extension.h"
#include "engine/resolutions.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "extension/windows.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>

namespace {
    std::unique_ptr<dx::World> freshWorld(const std::string& map)
    {
        dx::engine::initialize(dx::test::gameData());
        dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
        dx::objects::unloadPackage(dx::Name(map));
        auto world = std::make_unique<dx::World>();
        world->inputHook = [](dx::UObject* player, float delta) {
            dx::console::readInput(player, delta);
        };
        REQUIRE(world->loadMap(map));
        return world;
    }

    void releaseWorld(std::unique_ptr<dx::World>& world, const std::string& map)
    {
        dx::console::resetInput(world->player());
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

    void tick(dx::World& world, int frames)
    {
        for (int i = 0; i < frames; i++) {
            world.tick(1.0f / 30.0f);
        }
    }

    dx::UObject* objectProperty(dx::UObject* object, const char* name)
    {
        dx::UProperty* property = object->cls->findProperty(dx::Name(name));
        REQUIRE(property != nullptr);
        return *reinterpret_cast<dx::UObject**>(object->data + property->offset);
    }

    bool startInfoLink(dx::UObject* player, const char* name)
    {
        dx::vm::Call call(player, dx::Name("StartDataLinkTransmission"));
        call.s(name);
        call.skip();
        call.run();
        return *reinterpret_cast<uint32_t*>(call.returnValue()) != 0;
    }
}

TEST_CASE("player climbs a ladder by walking into it", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    REQUIRE(player != nullptr);
    dx::console::execute(player, "teleport 6040 -1742 260 16384");
    tick(*world, 10);
    float startZ = dx::actor::Location(player).z;
    CHECK(startZ < 270.0f);
    // The teleport leaves the player falling, and as in Deus Ex the ladder catches them: they
    // hang on it without input, still falling as far as the physics mode goes.
    CHECK(dx::actor::Physics(player) == 2);
    tick(*world, 10);
    CHECK(dx::actor::Location(player).z == startZ);
    uint8_t forward = dx::console::keyFromName("W");
    dx::console::key(forward, dx::IST_Press, 0);
    tick(*world, 30);
    dx::console::key(forward, dx::IST_Release, 0);
    float climbed = dx::actor::Location(player).z - startZ;
    CHECK(climbed > 60.0f);
    tick(*world, 30);
    CHECK(dx::actor::Location(player).z - startZ > 60.0f);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("climbing off the top of a ladder carries the player onto the floor without dropping back", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::console::execute(player, "teleport 6040 -1742 260 16384");
    tick(*world, 10);
    uint8_t forward = dx::console::keyFromName("W");
    dx::console::key(forward, dx::IST_Press, 0);
    float highest = dx::actor::Location(player).z;
    float sank = 0;
    // Until just past the top: the floor beyond ends in another drop.
    for (int i = 0; i < 120 && dx::actor::Location(player).y < -1700.0f; i++) {
        tick(*world, 1);
        float z = dx::actor::Location(player).z;
        sank = std::max(sank, highest - z);
        highest = std::max(highest, z);
    }
    dx::console::key(forward, dx::IST_Release, 0);
    // The ladder's face is at y -1716 and the floor it leads to is at z 400. Past the last
    // rung the player falls the short way onto that floor, as in Deus Ex, but never back
    // down the ladder.
    CHECK(sank < 2.0f);
    CHECK(dx::actor::Location(player).y > -1700.0f);
    CHECK(dx::actor::Location(player).z > 440.0f);
    tick(*world, 10);
    CHECK(dx::actor::Physics(player) == 1);
    CHECK(dx::actor::Location(player).z > 440.0f);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("traces do not report actors behind level geometry", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("04_NYC_NSFHQ");
    dx::UObject* barrel = findActor(*world, "Barrel64");
    REQUIRE(barrel != nullptr);
    dx::Vector start { 61.99f, 1328.6f, -240.0f };
    dx::Vector end { 61.99f, 6328.6f, -240.0f };
    std::vector<dx::CheckResult> hits = world->multiLineCheck(start, end, {}, nullptr, dx::TRACE_AllColliding);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits.front().actor == world->levelInfo);
    bool barrelHit = false;
    for (const dx::CheckResult& hit : hits) {
        if (hit.actor == barrel) {
            barrelHit = true;
        }
    }
    CHECK_FALSE(barrelHit);
    std::vector<dx::CheckResult> actorsOnly = world->multiLineCheck(start, end, {}, nullptr, dx::TRACE_AllColliding & ~(dx::TRACE_Level | dx::TRACE_LevelGeometry));
    bool barrelWithoutLevel = false;
    for (const dx::CheckResult& hit : actorsOnly) {
        if (hit.actor == barrel) {
            barrelWithoutLevel = true;
        }
    }
    CHECK(barrelWithoutLevel);
    releaseWorld(world, "04_NYC_NSFHQ");
}

TEST_CASE("long traces find actors just in front of the wall they end on", "[data]")
{
    // ULevel::MultiLineCheck checks only movers when the level is hit in the first 1% of the
    // trace and under 30 units away. A laser beam 5000 units long that ends 40 units away on
    // a wall still finds what stands in front of the wall.
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::CheckResult wall;
    REQUIRE(world->lineCheck(wall, { 6040, -1742, 300 }, { 6040, -1600, 300 }, {}, nullptr, dx::TRACE_World));
    float face = wall.location.y;
    dx::UClass* cellClass = dx::objects::findClass("DeusEx.BioelectricCell");
    REQUIRE(cellClass != nullptr);
    dx::UObject* cell = world->spawn(cellClass, nullptr, dx::Name(), { 6040, face - 10, 300 }, {}, nullptr, true);
    REQUIRE(cell != nullptr);
    auto firstHit = [&](float startDistance) -> dx::UObject* {
        dx::Vector start { 6040, face - startDistance, 300 };
        std::vector<dx::CheckResult> hits = world->multiLineCheck(start, start + dx::Vector { 0, 5000, 0 }, {}, nullptr, dx::TRACE_AllColliding);
        return hits.empty() ? nullptr : hits.front().actor;
    };
    CHECK(firstHit(40) == cell);
    // Closer than 30 units the original sees only the wall.
    CHECK(firstHit(20) == world->levelInfo);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("a box fits against a wall until it reaches the wall's face", "[data]")
{
    // UModel::PointCheck: a hull plane keeps the box out once the box's push-out is no more
    // than its distance in front, and the hull's bounding box, pushed out 0.1, does the same.
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::CheckResult wall;
    REQUIRE(world->lineCheck(wall, { 6040, -1742, 300 }, { 6040, -1600, 300 }, {}, nullptr, dx::TRACE_World));
    // A trace stops half a unit short of what it hits.
    float face = wall.location.y + 0.5f;
    dx::Vector extent { 10, 10, 10 };
    dx::CheckResult hit;
    CHECK_FALSE(world->pointCheck(hit, { 6040, face - 10.0f, 300 }, extent, nullptr, false));
    CHECK(world->pointCheck(hit, { 6040, face - 9.75f, 300 }, extent, nullptr, false));
    CHECK(hit.actor == world->levelInfo);
    // A point is inside only once it is past the face (the plate here is about 3 units thick).
    CHECK_FALSE(world->pointCheck(hit, { 6040, face - 0.25f, 300 }, {}, nullptr, false));
    CHECK(world->pointCheck(hit, { 6040, face + 1.0f, 300 }, {}, nullptr, false));
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("level traces report the texture of the surface that was hit", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::CheckResult hit;
    REQUIRE(world->lineCheck(hit, { 6040, -1742, 300 }, { 6040, -1600, 300 }, {}, nullptr, dx::TRACE_World));
    REQUIRE(hit.material != nullptr);
    CHECK(hit.material->name == dx::Name("LadrBrwnMetal"));
    REQUIRE(hit.material->outer != nullptr);
    CHECK(hit.material->outer->name == dx::Name("Ladder"));
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("player swims in a water zone", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    REQUIRE(player != nullptr);
    dx::console::execute(player, "teleport -2802 6641 -480 0");
    tick(*world, 20);
    REQUIRE(dx::region(player).zone != nullptr);
    CHECK(dx::region(player).zone->name == dx::Name("WaterZone1"));
    CHECK(dx::actor::Physics(player) == 3);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("swimming up against a low ledge jumps the player out of the water", "[data]")
{
    // The end of the training pool, where the floor is a little above the water.
    std::unique_ptr<dx::World> world = freshWorld("00_Training");
    dx::UObject* player = world->player();
    dx::console::execute(player, "teleport 5480 -4000 -90 32768");
    tick(*world, 30);
    REQUIRE(dx::actor::Physics(player) == 3);
    uint8_t forward = dx::console::keyFromName("W");
    dx::console::key(forward, dx::IST_Press, 0);
    float lowest = 0;
    for (int i = 0; i < 80; i++) {
        // Looking up a little, as the water jump needs.
        dx::pawn::ViewRotation(player).pitch = 6000;
        tick(*world, 1);
        lowest = std::min(lowest, dx::actor::Velocity(player).z);
    }
    dx::console::key(forward, dx::IST_Release, 0);
    tick(*world, 20);
    CHECK(dx::actor::Physics(player) == 1);
    CHECK(dx::actor::Location(player).z > -10.0f);
    CHECK(dx::actor::CollisionHeight(player) > 40.0f);
    CHECK(lowest > -500.0f);
    releaseWorld(world, "00_Training");
}

TEST_CASE("frobbing an unlocked door opens it", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::UObject* door = findActor(*world, "DeusExMover9");
    REQUIRE(door != nullptr);
    dx::Vector closed = dx::actor::Location(door);
    dx::Rotator closedRotation = dx::actor::Rotation(door);
    dx::vm::Call frob(door, dx::Name("Frob"));
    REQUIRE(frob.valid());
    frob.obj(player).obj(nullptr).run();
    tick(*world, 90);
    dx::Vector moved = dx::actor::Location(door) - closed;
    dx::Rotator turned = dx::actor::Rotation(door);
    bool changed = moved.sizeSquared() > 1.0f || turned.yaw != closedRotation.yaw || turned.pitch != closedRotation.pitch;
    CHECK(changed);
    CHECK(dx::mover::KeyNum(door) == 1);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("each mission starts its mission script with a living player", "[data]")
{
    const std::pair<const char*, const char*> missions[] = {
        { "00_Training", "Mission00" },
        { "01_NYC_UNATCOIsland", "Mission01" },
        { "02_NYC_BatteryPark", "Mission02" },
        { "03_NYC_UNATCOIsland", "Mission03" },
        { "04_NYC_UNATCOIsland", "Mission04" },
        { "05_NYC_UNATCOMJ12lab", "Mission05" },
        { "06_HongKong_Helibase", "Mission06" },
        { "08_NYC_Street", "Mission08" },
        { "09_NYC_Dockyard", "Mission09" },
        { "10_Paris_Catacombs", "Mission10" },
        { "11_Paris_Cathedral", "Mission11" },
        { "12_Vandenberg_Cmd", "Mission12" },
        { "14_Vandenberg_Sub", "Mission14" },
        { "15_Area51_Bunker", "Mission15" },
        { "99_Endgame1", "MissionEndgame" },
    };
    for (const auto& [map, script] : missions) {
        INFO(map);
        std::unique_ptr<dx::World> world = freshWorld(map);
        tick(*world, 60);
        dx::UObject* mission = nullptr;
        for (dx::UObject* a : world->actors) {
            if (a && !a->destroyed && a->isA(dx::Name("MissionScript"))) {
                mission = a;
            }
        }
        REQUIRE(mission != nullptr);
        CHECK(mission->cls->name == dx::Name(script));
        REQUIRE(world->player() != nullptr);
        CHECK(dx::pawn::Health(world->player()) > 0);
        releaseWorld(world, map);
    }
}

TEST_CASE("animals looking for food do not stall the ocean lab", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("14_OceanLab_Lab");
    auto start = std::chrono::steady_clock::now();
    tick(*world, 300);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(seconds < 60.0);
    releaseWorld(world, "14_OceanLab_Lab");
}

TEST_CASE("picking a weak lock with one lockpick unlocks the door and uses the pick", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::UObject* door = findActor(*world, "DeusExMover10");
    REQUIRE(door != nullptr);
    dx::UProperty* locked = door->cls->findProperty(dx::Name("bLocked"));
    REQUIRE(locked != nullptr);
    REQUIRE(locked->toText(door->data + locked->offset) == "True");
    dx::console::execute(player, "teleport 1872 -1808 -100 16384");
    dx::console::execute(player, "lookat 1872 -1748 -80");
    dx::console::execute(player, "giveitem DeusEx.Lockpick");
    dx::UObject* pick = nullptr;
    for (dx::UObject* item = dx::actor::Inventory(player); item; item = dx::actor::Inventory(item)) {
        if (item->isA(dx::Name("Lockpick"))) {
            pick = item;
        }
    }
    REQUIRE(pick != nullptr);
    dx::vm::Call(player, dx::Name("PutInHand")).obj(pick).run();
    tick(*world, 15);
    dx::vm::Call(door, dx::Name("Frob")).obj(player).obj(pick).run();
    tick(*world, 240);
    CHECK(locked->toText(door->data + locked->offset) == "False");
    bool stillHasPick = false;
    for (dx::UObject* item = dx::actor::Inventory(player); item; item = dx::actor::Inventory(item)) {
        if (item->isA(dx::Name("Lockpick"))) {
            stillHasPick = true;
        }
    }
    CHECK_FALSE(stillHasPick);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("putting an item in hand does not close the open inventory screen", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::console::execute(player, "giveitem DeusEx.Lockpick");
    dx::console::execute(player, "giveitem DeusEx.Medkit");
    dx::vm::Call(player, dx::Name("ShowInventoryWindow")).run();
    tick(*world, 5);
    dx::UProperty* rootProperty = player->cls->findProperty(dx::Name("rootWindow"));
    dx::UObject* root = *reinterpret_cast<dx::UObject**>(player->data + rootProperty->offset);
    REQUIRE(root != nullptr);
    dx::vm::Call top(root, dx::Name("GetTopWindow"));
    REQUIRE(top.valid());
    top.run();
    dx::UObject* screen = *reinterpret_cast<dx::UObject**>(top.returnValue());
    REQUIRE(screen != nullptr);
    CHECK(screen->cls->name == dx::Name("PersonaScreenInventory"));
    for (dx::UObject* item = dx::actor::Inventory(player); item; item = dx::actor::Inventory(item)) {
        if (item->isA(dx::Name("Lockpick")) || item->isA(dx::Name("MedKit"))) {
            dx::vm::Call(player, dx::Name("PutInHand")).obj(item).run();
            tick(*world, 5);
        }
    }
    dx::vm::Call after(root, dx::Name("GetTopWindow"));
    after.run();
    CHECK(*reinterpret_cast<dx::UObject**>(after.returnValue()) == screen);
    CHECK_FALSE(screen->destroyed);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("both A and X place a moving inventory item and appear in its prompts", "[gamepad][data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::console::execute(world->player(), "giveitem DeusEx.Medkit");
    dx::vm::Call(world->player(), dx::Name("ShowInventoryWindow")).run();
    tick(*world, 5);
    dx::UObject* screen = dx::extension::topModal();
    REQUIRE(dx::script::isA(screen, "PersonaScreenInventory"));
    auto* grid = dx::cast<dx::extension::UWindow>(objectProperty(screen, "winItems"));
    REQUIRE(grid != nullptr);
    dx::UObject* selected = nullptr;
    for (auto* button = grid->firstChild(); button; button = button->nextSibling()) {
        if (dx::script::isA(button, "PersonaInventoryItemButton") && dx::script::flag(button, "bAllowDragging")) {
            selected = button;
            break;
        }
    }
    REQUIRE(selected != nullptr);
    dx::extension::setFocusWindow(selected);

    for (dx::gamepad::Button place : { dx::gamepad::A, dx::gamepad::X }) {
        REQUIRE(dx::gamepadmenu::press(dx::gamepad::X));
        REQUIRE(dx::padinventory::busy());
        std::vector<dx::prompts::Prompt> prompts = dx::gamepadmenu::prompts();
        for (dx::gamepad::Button button : { dx::gamepad::A, dx::gamepad::X }) {
            CHECK(std::any_of(prompts.begin(), prompts.end(), [button](const auto& prompt) { return dx::gamepad::button(prompt.action) == button || dx::gamepad::alternative(prompt.action).button == button; }));
        }
        CHECK(std::any_of(prompts.begin(), prompts.end(), [](const auto& prompt) { return prompt.label == "Place"; }));
        CHECK(std::any_of(prompts.begin(), prompts.end(), [](const auto& prompt) { return dx::gamepad::button(prompt.action) == dx::gamepad::A && dx::gamepad::alternative(prompt.action).button == dx::gamepad::X && prompt.label == "Place"; }));
        CHECK(std::any_of(prompts.begin(), prompts.end(), [](const auto& prompt) { return std::string(dx::gamepad::glyphName(dx::gamepad::control(prompt.action))) == "pad_ls" && std::string(dx::gamepad::glyphName(dx::gamepad::alternative(prompt.action))) == "pad_dpad" && prompt.label == "Move"; }));
        REQUIRE(dx::gamepadmenu::press(place));
        CHECK_FALSE(dx::padinventory::busy());
        CHECK(dx::extension::topModal() == screen);
    }
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("inventory equip and drop follow gamepad focus without making use automatic", "[gamepad][data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::console::execute(player, "giveitem DeusEx.Medkit");
    dx::console::execute(player, "giveitem DeusEx.Lockpick");
    dx::vm::Call(player, dx::Name("ShowInventoryWindow")).run();
    tick(*world, 5);
    dx::UObject* screen = dx::extension::topModal();
    REQUIRE(dx::script::isA(screen, "PersonaScreenInventory"));
    auto* grid = dx::cast<dx::extension::UWindow>(objectProperty(screen, "winItems"));
    REQUIRE(grid != nullptr);
    dx::UObject* medkit = nullptr;
    dx::UObject* lockpick = nullptr;
    for (auto* button = grid->firstChild(); button; button = button->nextSibling()) {
        dx::UObject* item = dx::script::object(button, "clientObject");
        if (dx::script::isA(item, "MedKit")) {
            medkit = button;
        } else if (dx::script::isA(item, "Lockpick")) {
            lockpick = button;
        }
    }
    REQUIRE(medkit != nullptr);
    REQUIRE(lockpick != nullptr);
    dx::UObject* medkitItem = objectProperty(medkit, "clientObject");
    dx::UObject* lockpickItem = objectProperty(lockpick, "clientObject");
    dx::vm::Call(screen, dx::Name("SelectInventory")).obj(medkit).run();
    dx::extension::setFocusWindow(lockpick);
    auto hasPrompt = [](dx::gamepad::Button button, const char* label) {
        auto prompts = dx::gamepadmenu::prompts();
        return std::any_of(prompts.begin(), prompts.end(), [=](const auto& prompt) { return dx::gamepad::button(prompt.action) == button && prompt.label == label; });
    };

    SECTION("equipping selects the highlighted item, not the old selection")
    {
        CHECK(hasPrompt(dx::gamepad::Y, "Equip"));
        CHECK(objectProperty(screen, "selectedItem") == medkit);
        REQUIRE(dx::gamepadmenu::press(dx::gamepad::Y));
        CHECK(objectProperty(screen, "selectedItem") == lockpick);
        CHECK(objectProperty(player, "inHandPending") == lockpickItem);
        CHECK(hasPrompt(dx::gamepad::Y, "Unequip"));
    }
    SECTION("unequipping follows focus and advertises Unequip before selection")
    {
        dx::vm::Call(player, dx::Name("PutInHand")).obj(lockpickItem).run();
        CHECK(hasPrompt(dx::gamepad::Y, "Unequip"));
        CHECK(objectProperty(screen, "selectedItem") == medkit);
        REQUIRE(dx::gamepadmenu::press(dx::gamepad::Y));
        CHECK(objectProperty(screen, "selectedItem") == lockpick);
        CHECK(objectProperty(player, "inHandPending") == nullptr);
        CHECK(hasPrompt(dx::gamepad::Y, "Equip"));
    }
    SECTION("dropping selects and drops the highlighted item")
    {
        CHECK(hasPrompt(dx::gamepad::LeftTrigger, "Drop"));
        CHECK(objectProperty(screen, "selectedItem") == medkit);
        REQUIRE(dx::gamepadmenu::press(dx::gamepad::LeftTrigger));
        auto items = dx::script::inventory(player);
        CHECK(std::find(items.begin(), items.end(), medkitItem) != items.end());
        CHECK(std::find(items.begin(), items.end(), lockpickItem) == items.end());
    }
    SECTION("using still requires explicitly selecting the highlighted item")
    {
        CHECK(hasPrompt(dx::gamepad::A, "Select"));
        CHECK_FALSE(dx::padinventory::usesOnA(screen));
        CHECK_FALSE(dx::gamepadmenu::press(dx::gamepad::A));
        CHECK(objectProperty(screen, "selectedItem") == medkit);
        dx::vm::Call(screen, dx::Name("SelectInventory")).obj(lockpick).run();
        CHECK(dx::padinventory::usesOnA(screen));
        CHECK(hasPrompt(dx::gamepad::A, "Use"));
    }
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("gamepad browsing reaches quickbar slots and clears assignments without dropping items", "[gamepad][data]")
{
    class LayoutCanvas : public dx::Canvas2D {
    public:
        float width() const override
        {
            return 1280;
        }

        float height() const override
        {
            return 800;
        }

        void setClip(float, float, float, float) override
        {
        }

        void clearClip() override
        {
        }

        void drawTile(dx::UTexture*, float, float, float, float, float, float, float, float, dx::LinearColor, dx::BlendStyle, bool) override
        {
        }

        void drawRect(float, float, float, float, dx::LinearColor, dx::BlendStyle) override
        {
        }

        void drawLine(float, float, float, float, dx::LinearColor) override
        {
        }
    } canvas;

    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::console::execute(player, "giveitem DeusEx.Medkit");
    dx::console::execute(player, "giveitem DeusEx.Lockpick");
    dx::vm::Call(player, dx::Name("ShowInventoryWindow")).run();
    tick(*world, 5);
    dx::UObject* screen = dx::extension::topModal();
    REQUIRE(dx::script::isA(screen, "PersonaScreenInventory"));
    auto* grid = dx::cast<dx::extension::UWindow>(objectProperty(screen, "winItems"));
    REQUIRE(grid != nullptr);
    dx::UObject* medkit = nullptr;
    dx::UObject* lockpick = nullptr;
    for (auto* button = grid->firstChild(); button; button = button->nextSibling()) {
        dx::UObject* item = dx::script::object(button, "clientObject");
        if (dx::script::isA(item, "MedKit")) {
            medkit = button;
        } else if (dx::script::isA(item, "Lockpick")) {
            lockpick = button;
        }
    }
    REQUIRE(medkit != nullptr);
    REQUIRE(lockpick != nullptr);
    dx::UObject* medkitItem = objectProperty(medkit, "clientObject");
    dx::UObject* lockpickItem = objectProperty(lockpick, "clientObject");
    dx::UObject* inventoryBelt = objectProperty(screen, "invBelt");
    dx::UObject* belt = objectProperty(inventoryBelt, "objBelt");
    dx::vm::Call(inventoryBelt, dx::Name("AddObject")).obj(medkitItem).i(1).run();
    dx::vm::Call(inventoryBelt, dx::Name("AddObject")).obj(lockpickItem).i(2).run();
    dx::UObject* first = dx::script::object(belt, "objects", 1);
    dx::UObject* second = dx::script::object(belt, "objects", 2);
    dx::UObject* empty = dx::script::object(belt, "objects", 3);
    dx::extension::render(canvas);
    dx::vm::Call(screen, dx::Name("SelectInventory")).obj(medkit).run();
    dx::extension::setFocusWindow(medkit);
    for (int i = 0; i < 20 && !dx::padinventory::beltFocused(screen); i++) {
        REQUIRE_FALSE(dx::padinventory::steer(dx::IK_Down));
        REQUIRE(dx::extension::navigate(dx::IK_Down));
    }
    REQUIRE(dx::padinventory::beltFocused(screen));
    CHECK_FALSE(dx::padinventory::busy());
    dx::extension::setFocusWindow(first);
    REQUIRE(dx::extension::navigate(dx::IK_Right));
    CHECK(dx::extension::focusWindow() == second);
    auto prompts = dx::gamepadmenu::prompts();
    CHECK(std::any_of(prompts.begin(), prompts.end(), [](const auto& prompt) { return dx::gamepad::button(prompt.action) == dx::gamepad::X && prompt.label == "Clear slot"; }));
    CHECK(std::none_of(prompts.begin(), prompts.end(), [](const auto& prompt) { return prompt.label == "Drop" || prompt.label == "Equip" || prompt.label == "Use"; }));
    REQUIRE(dx::gamepadmenu::press(dx::gamepad::X));
    CHECK(dx::script::object(second, "item") == nullptr);
    CHECK(dx::extension::focusWindow() == second);
    CHECK(dx::script::object(first, "item") == medkitItem);
    CHECK_FALSE(dx::script::flag(lockpickItem, "bInObjectBelt"));
    CHECK(dx::script::integer(lockpickItem, "beltPos") == -1);
    auto items = dx::script::inventory(player);
    CHECK(std::find(items.begin(), items.end(), lockpickItem) != items.end());
    dx::UObject* hudBelt = dx::script::object(dx::script::object(dx::extension::rootWindow(), "hud"), "belt");
    CHECK(dx::script::object(dx::script::object(hudBelt, "objects", 2), "item") == nullptr);
    REQUIRE(dx::extension::navigate(dx::IK_Right));
    CHECK(dx::extension::focusWindow() == empty);
    REQUIRE(dx::gamepadmenu::press(dx::gamepad::X));
    CHECK(dx::script::object(empty, "item") == nullptr);
    prompts = dx::gamepadmenu::prompts();
    CHECK(std::none_of(prompts.begin(), prompts.end(), [](const auto& prompt) { return prompt.label == "Clear slot"; }));
    dx::UObject* keySlot = dx::script::object(belt, "objects", 0);
    dx::UObject* keyring = dx::script::object(keySlot, "item");
    REQUIRE(dx::script::isA(keyring, "NanoKeyRing"));
    dx::extension::setFocusWindow(keySlot);
    REQUIRE(dx::gamepadmenu::press(dx::gamepad::X));
    CHECK(dx::script::object(keySlot, "item") == keyring);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("shooting an explosive barrel destroys it and hurts a nearby player", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::UObject* barrel = findActor(*world, "Barrel0");
    REQUIRE(barrel != nullptr);
    dx::Vector barrelLocation = dx::actor::Location(barrel);
    dx::console::execute(player, "teleport 1433 -10144 -260 32768");
    tick(*world, 5);
    REQUIRE((dx::actor::Location(player) - barrelLocation).size2D() < 120.0f);
    int startHealth = dx::pawn::Health(player);
    dx::vm::Call damage(barrel, dx::Name("TakeDamage"));
    REQUIRE(damage.valid());
    damage.i(1000).obj(player).v(barrelLocation).v({}).n(dx::Name("Shot")).run();
    tick(*world, 60);
    CHECK((barrel->destroyed || dx::actor::bDeleteMe(barrel)));
    CHECK(dx::pawn::Health(player) < startHealth);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("killing a terrorist leaves a carcass whose items the player can take", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::UObject* terrorist = findActor(*world, "Terrorist1");
    REQUIRE(terrorist != nullptr);
    tick(*world, 30);
    REQUIRE(dx::actor::Inventory(terrorist) != nullptr);
    std::vector<dx::UObject*> existing;
    for (dx::UObject* a : world->actors) {
        if (a && !a->destroyed && a->isA(dx::Name("DeusExCarcass"))) {
            existing.push_back(a);
        }
    }
    dx::vm::Call damage(terrorist, dx::Name("TakeDamage"));
    REQUIRE(damage.valid());
    damage.i(150).obj(player).v(dx::actor::Location(terrorist)).v({}).n(dx::Name("Shot")).run();
    tick(*world, 150);
    CHECK(terrorist->destroyed);
    dx::UObject* carcass = nullptr;
    for (dx::UObject* a : world->actors) {
        if (a && !a->destroyed && a->isA(dx::Name("TerroristCarcass")) && std::find(existing.begin(), existing.end(), a) == existing.end()) {
            carcass = a;
        }
    }
    REQUIRE(carcass != nullptr);
    dx::Vector spot = dx::actor::Location(carcass);
    dx::console::execute(player, "teleport " + std::to_string(spot.x + 60) + " " + std::to_string(spot.y) + " " + std::to_string(spot.z + 30) + " 32768");
    dx::vm::Call(carcass, dx::Name("Frob")).obj(player).obj(nullptr).run();
    tick(*world, 5);
    bool gotItem = false;
    for (dx::UObject* item = dx::actor::Inventory(player); item; item = dx::actor::Inventory(item)) {
        if (item->isA(dx::Name("WeaponRifle")) || item->isA(dx::Name("Ammo3006")) || item->isA(dx::Name("WeaponCombatKnife")) || item->isA(dx::Name("TechGoggles"))) {
            gotItem = true;
        }
    }
    CHECK(gotItem);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("the use prompt on a body says Search until it is empty and then Pick up", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::UObject* terrorist = findActor(*world, "Terrorist1");
    REQUIRE(terrorist != nullptr);
    tick(*world, 30);
    std::vector<dx::UObject*> existing;
    for (dx::UObject* a : world->actors) {
        if (a && !a->destroyed && a->isA(dx::Name("DeusExCarcass"))) {
            existing.push_back(a);
        }
    }
    dx::vm::Call(terrorist, dx::Name("TakeDamage")).i(150).obj(player).v(dx::actor::Location(terrorist)).v({}).n(dx::Name("Shot")).run();
    tick(*world, 150);
    dx::UObject* carcass = nullptr;
    for (dx::UObject* a : world->actors) {
        if (a && !a->destroyed && a->isA(dx::Name("TerroristCarcass")) && std::find(existing.begin(), existing.end(), a) == existing.end()) {
            carcass = a;
        }
    }
    REQUIRE(carcass != nullptr);
    REQUIRE(dx::actor::Inventory(carcass) != nullptr);
    CHECK(std::string(dx::prompts::interactionLabel(carcass, player)) == "Search");
    dx::Vector spot = dx::actor::Location(carcass);
    dx::console::execute(player, "teleport " + std::to_string(spot.x + 60) + " " + std::to_string(spot.y) + " " + std::to_string(spot.z + 30) + " 32768");
    dx::vm::Call(carcass, dx::Name("Frob")).obj(player).obj(nullptr).run();
    tick(*world, 5);
    REQUIRE(dx::actor::Inventory(carcass) == nullptr);
    CHECK(std::string(dx::prompts::interactionLabel(carcass, player)) == "Pick up");
    // With something in hand the body is searched again rather than carried.
    dx::UObject** inHand = reinterpret_cast<dx::UObject**>(player->propertyAddress(dx::Name("inHand")));
    *inHand = dx::actor::Inventory(player);
    REQUIRE(*inHand != nullptr);
    CHECK(std::string(dx::prompts::interactionLabel(carcass, player)) == "Search");
    *inHand = nullptr;
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("player picks up a light decoration and throws it", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::console::execute(player, "teleport -4300 10400 -259 32768");
    tick(*world, 5);
    dx::Vector start = dx::actor::Location(player);
    dx::UObject* box = world->spawn(dx::objects::findClass("DeusEx.BoxSmall"), nullptr, dx::Name(), start + dx::Vector { -60, 0, -30 }, {}, nullptr, false);
    REQUIRE(box != nullptr);
    tick(*world, 20);
    dx::Vector spot = dx::actor::Location(box);
    dx::console::execute(player, "lookat " + std::to_string(spot.x) + " " + std::to_string(spot.y) + " " + std::to_string(spot.z));
    tick(*world, 10);
    dx::UProperty* frobTarget = player->cls->findProperty(dx::Name("FrobTarget"));
    REQUIRE(*reinterpret_cast<dx::UObject**>(player->data + frobTarget->offset) == box);
    dx::vm::Call(player, dx::Name("GrabDecoration")).run();
    tick(*world, 10);
    dx::UProperty* carried = player->cls->findProperty(dx::Name("CarriedDecoration"));
    REQUIRE(*reinterpret_cast<dx::UObject**>(player->data + carried->offset) == box);
    dx::console::execute(player, "lookat -4600 10400 -200");
    tick(*world, 5);
    dx::vm::Call(player, dx::Name("DropDecoration")).run();
    CHECK(*reinterpret_cast<dx::UObject**>(player->data + carried->offset) == nullptr);
    CHECK(dx::actor::Physics(box) == 2);
    tick(*world, 30);
    CHECK(dx::actor::Location(box).x < start.x - 80.0f);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("following interpolation points sets the game speed and skips points marked bSkipNextPath", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UClass* pointClass = dx::objects::findClass("Engine.InterpolationPoint");
    REQUIRE(pointClass != nullptr);
    auto set = [](dx::UObject* o, const char* name, auto value) {
        uint8_t* address = o->propertyAddress(dx::Name(name));
        REQUIRE(address != nullptr);
        *reinterpret_cast<decltype(value)*>(address) = value;
    };
    dx::Vector origin { -4300, 10400, -100 };
    dx::UObject* points[4] = {};
    for (int i = 0; i < 4; i++) {
        points[i] = world->spawn(pointClass, nullptr, dx::Name(), origin + dx::Vector { 64.0f * static_cast<float>(i), 0, 0 }, {}, nullptr, false);
        REQUIRE(points[i] != nullptr);
    }
    for (int i = 0; i < 4; i++) {
        set(points[i], "Next", points[(i + 1) % 4]);
        set(points[i], "Prev", points[(i + 3) % 4]);
    }
    set(points[0], "GameSpeedModifier", 1.0f);
    set(points[1], "GameSpeedModifier", 0.5f);
    set(points[1], "bSkipNextPath", uint32_t { 1 });

    dx::UObject* mover = world->spawn(dx::objects::findClass("DeusEx.BoxSmall"), nullptr, dx::Name(), origin, {}, nullptr, false);
    REQUIRE(mover != nullptr);
    dx::vm::Call(mover, dx::Name("SetCollision")).b(false).b(false).b(false).run();
    dx::actor::bCollideWorld(mover) = 0;
    dx::actor::Target(mover) = points[0];
    dx::actor::PhysRate(mover) = 1.0f;
    dx::actor::PhysAlpha(mover) = 0.0f;
    dx::actor::bInterpolating(mover) = 1;
    dx::vm::Call(mover, dx::Name("SetPhysics")).byte(dx::PHYS_Interpolating).run();

    // Part way from point 0 to point 1 the game speed is blended between them.
    tick(*world, 10);
    float dilation = dx::levelinfo::TimeDilation(world->levelInfo);
    CHECK(dilation < 1.0f);
    CHECK(dilation > 0.5f);
    CHECK(dx::actor::Target(mover) == points[0]);

    // Arriving at point 1, which is marked bSkipNextPath, moves straight on to point 2.
    for (int i = 0; i < 120 && dx::actor::Target(mover) == points[0]; i++) {
        tick(*world, 1);
    }
    CHECK(dx::actor::Target(mover) == points[2]);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("a carried decoration stays in front of the player through a full turn", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::console::execute(player, "teleport -4300 10400 -259 32768");
    tick(*world, 5);
    dx::Vector start = dx::actor::Location(player);
    dx::UObject* box = world->spawn(dx::objects::findClass("DeusEx.BoxSmall"), nullptr, dx::Name(), start + dx::Vector { -60, 0, -30 }, {}, nullptr, false);
    REQUIRE(box != nullptr);
    tick(*world, 20);
    dx::Vector spot = dx::actor::Location(box);
    dx::console::execute(player, "lookat " + std::to_string(spot.x) + " " + std::to_string(spot.y) + " " + std::to_string(spot.z));
    tick(*world, 10);
    dx::vm::Call(player, dx::Name("GrabDecoration")).run();
    tick(*world, 5);
    dx::UProperty* carried = player->cls->findProperty(dx::Name("CarriedDecoration"));
    REQUIRE(*reinterpret_cast<dx::UObject**>(player->data + carried->offset) == box);
    // Turn round in steps of 1/16 of a turn per frame, as the mouse does: input turns ViewRotation, and
    // the player's UpdateRotation turns the pawn with SetRotation, which swings what it is carrying.
    for (int step = 1; step <= 16; step++) {
        dx::pawn::ViewRotation(player).yaw += 4096;
        tick(*world, 1);
        INFO("step " << step);
        REQUIRE(*reinterpret_cast<dx::UObject**>(player->data + carried->offset) == box);
        float angle = static_cast<float>(dx::actor::Rotation(player).yaw) * 3.14159265f / 32768.0f;
        dx::Vector offset = dx::actor::Location(box) - dx::actor::Location(player);
        CHECK(offset.x * std::cos(angle) + offset.y * std::sin(angle) > 0.0f);
    }
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("a carried decoration stays carried while the mouse turns the player", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::console::execute(player, "teleport -4300 10400 -259 32768");
    tick(*world, 5);
    dx::Vector start = dx::actor::Location(player);
    dx::UObject* box = world->spawn(dx::objects::findClass("DeusEx.BoxSmall"), nullptr, dx::Name(), start + dx::Vector { -60, 0, -30 }, {}, nullptr, false);
    REQUIRE(box != nullptr);
    tick(*world, 20);
    dx::Vector spot = dx::actor::Location(box);
    dx::console::execute(player, "lookat " + std::to_string(spot.x) + " " + std::to_string(spot.y) + " " + std::to_string(spot.z));
    tick(*world, 10);
    dx::vm::Call(player, dx::Name("GrabDecoration")).run();
    tick(*world, 10);
    dx::UProperty* carried = player->cls->findProperty(dx::Name("CarriedDecoration"));
    REQUIRE(*reinterpret_cast<dx::UObject**>(player->data + carried->offset) == box);
    REQUIRE(dx::actor::Base(box) == player);
    int32_t startYaw = dx::actor::Rotation(player).yaw;
    for (int i = 0; i < 30; i++) {
        dx::console::key(dx::IK_MouseX, dx::IST_Axis, 20.0f);
        tick(*world, 1);
    }
    REQUIRE(dx::actor::Rotation(player).yaw != startYaw);
    CHECK(*reinterpret_cast<dx::UObject**>(player->data + carried->offset) == box);
    CHECK(dx::actor::Base(box) == player);
    CHECK(dx::actor::Physics(box) == 0);
    CHECK_FALSE(dx::actor::bCollideActors(box));
    int32_t boxYaw = dx::vectorRotation(dx::actor::Location(box) - dx::actor::Location(player)).yaw;
    int32_t offset = ((boxYaw & 65535) - (dx::actor::Rotation(player).yaw & 65535)) & 65535;
    CHECK((offset <= 3072 || offset >= 62463));
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("a security camera spots the player and raises the alarm", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::UObject* camera = findActor(*world, "SecurityCamera3");
    REQUIRE(camera != nullptr);
    dx::UProperty* seen = camera->cls->findProperty(dx::Name("bPlayerSeen"));
    dx::UProperty* triggered = camera->cls->findProperty(dx::Name("bEventTriggered"));
    CHECK(seen->toText(camera->data + seen->offset) == "False");
    dx::console::execute(player, "teleport 191 223 -60 0");
    tick(*world, 180);
    CHECK(seen->toText(camera->data + seen->offset) == "True");
    CHECK(triggered->toText(camera->data + triggered->offset) == "True");
    REQUIRE(dx::actor::AmbientSound(camera) != nullptr);
    CHECK(dx::actor::AmbientSound(camera)->name == dx::Name("Klaxon2"));
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("a patrolling terrorist reacts to a nearby explosion", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::UObject* crate = findActor(*world, "CrateExplosiveSmall10");
    dx::UObject* terrorist = findActor(*world, "Terrorist2");
    REQUIRE(crate != nullptr);
    REQUIRE(terrorist != nullptr);
    for (int i = 0; i < 60 && dx::vm::stateName(terrorist) != dx::Name("Patrolling"); i++) {
        dx::actor::LastRenderTime(terrorist) = world->timeSeconds;
        tick(*world, 1);
    }
    REQUIRE(dx::vm::stateName(terrorist) == dx::Name("Patrolling"));
    dx::actor::LastRenderTime(terrorist) = world->timeSeconds;
    dx::vm::Call damage(crate, dx::Name("TakeDamage"));
    damage.i(1000).obj(player).v(dx::actor::Location(crate)).v({}).n(dx::Name("Shot")).run();
    bool reacted = false;
    for (int i = 0; i < 90 && !reacted; i++) {
        tick(*world, 1);
        reacted = dx::vm::stateName(terrorist) != dx::Name("Patrolling");
    }
    INFO(dx::vm::stateName(terrorist).str());
    CHECK(reacted);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("a door's bounding box spans the faces that traces hit from both sides", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* door = findActor(*world, "DeusExMover10");
    REQUIRE(door != nullptr);
    dx::CheckResult front;
    REQUIRE(world->lineCheck(front, { 1872, -1808, -60 }, { 1872, -1608, -60 }, {}, nullptr, dx::TRACE_Movers));
    REQUIRE(front.actor == door);
    dx::CheckResult back;
    REQUIRE(world->lineCheck(back, { 1872, -1608, -60 }, { 1872, -1808, -60 }, {}, nullptr, dx::TRACE_Movers));
    REQUIRE(back.actor == door);
    dx::vm::Call box(door, dx::Name("GetBoundingBox"));
    REQUIRE(box.valid());
    box.v({}).v({}).b(false).v(dx::actor::Location(door)).r(dx::actor::Rotation(door));
    box.run();
    dx::Vector minimum = *reinterpret_cast<dx::Vector*>(box.parameter(0));
    dx::Vector maximum = *reinterpret_cast<dx::Vector*>(box.parameter(1));
    CHECK(minimum.y == Catch::Approx(front.location.y).margin(1.0));
    CHECK(maximum.y == Catch::Approx(back.location.y).margin(1.0));
    CHECK(minimum.x <= 1872.0f);
    CHECK(maximum.x >= 1872.0f);
    CHECK(minimum.z <= -60.0f);
    CHECK(maximum.z >= -60.0f);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("a queued infolink transmission shows its own HUD window", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    tick(*world, 30);
    dx::UObject* hud = objectProperty(objectProperty(player, "rootWindow"), "hud");
    REQUIRE(startInfoLink(player, "DL_GateEntrance"));
    dx::UObject* play = objectProperty(player, "dataLinkPlay");
    REQUIRE(play != nullptr);
    dx::UObject* firstWindow = objectProperty(hud, "infolink");
    REQUIRE(firstWindow != nullptr);
    tick(*world, 30);
    REQUIRE(startInfoLink(player, "DL_Top"));
    dx::UProperty* conName = dx::objects::findClass("ConSys.Conversation")->findProperty(dx::Name("conName"));
    REQUIRE(conName != nullptr);
    bool playingSecond = false;
    for (int i = 0; i < 900 && !playingSecond; i++) {
        tick(*world, 1);
        dx::UObject* con = objectProperty(play, "con");
        playingSecond = con && conName->toText(con->data + conName->offset) == "DL_Top" && dx::vm::stateName(play) == dx::Name("WaitForSpeech");
    }
    REQUIRE(playingSecond);
    CHECK(firstWindow->destroyed);
    dx::UObject* secondWindow = objectProperty(play, "datalink");
    REQUIRE(secondWindow != nullptr);
    CHECK_FALSE(secondWindow->destroyed);
    CHECK(objectProperty(hud, "infolink") == secondWindow);
    dx::UProperty* visible = secondWindow->cls->findProperty(dx::Name("bIsVisible"));
    CHECK(visible->toText(secondWindow->data + visible->offset) == "True");
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("an infolink queued during a conversation shows its HUD window when the conversation ends", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    tick(*world, 30);
    dx::UObject* hud = objectProperty(objectProperty(player, "rootWindow"), "hud");
    dx::UProperty* hudVisible = hud->cls->findProperty(dx::Name("bIsVisible"));
    dx::UObject* paul = nullptr;
    for (dx::UObject* a : world->actors) {
        if (a && !a->destroyed && a->isA(dx::Name("PaulDenton"))) {
            paul = a;
        }
    }
    REQUIRE(paul != nullptr);
    REQUIRE(startInfoLink(player, "DL_GateEntrance"));
    dx::UObject* firstWindow = objectProperty(hud, "infolink");
    REQUIRE(firstWindow != nullptr);
    tick(*world, 60);
    dx::Vector spot = dx::actor::Location(paul);
    dx::console::execute(player, "teleport " + std::to_string(spot.x + 80) + " " + std::to_string(spot.y) + " " + std::to_string(spot.z) + " 32768");
    dx::vm::Call conversation(player, dx::Name("StartConversationByName"));
    conversation.n(dx::Name("MeetPaul")).obj(paul).b(false).b(false);
    conversation.run();
    REQUIRE(*reinterpret_cast<uint32_t*>(conversation.returnValue()) != 0);
    dx::UObject* conPlay = objectProperty(player, "conPlay");
    REQUIRE(conPlay != nullptr);
    tick(*world, 30);
    REQUIRE(hudVisible->toText(hud->data + hudVisible->offset) == "False");
    startInfoLink(player, "DL_Top");
    tick(*world, 30);
    dx::UObject* play = objectProperty(player, "dataLinkPlay");
    REQUIRE(play != nullptr);
    REQUIRE(objectProperty(player, "conPlay") == conPlay);
    dx::UProperty* conName = dx::objects::findClass("ConSys.Conversation")->findProperty(dx::Name("conName"));
    dx::UObject* queued = objectProperty(play, "con");
    REQUIRE(queued != nullptr);
    REQUIRE(conName->toText(queued->data + conName->offset) == "DL_Top");
    dx::vm::Call(conPlay, dx::Name("TerminateConversation")).run();
    for (int i = 0; i < 300 && objectProperty(player, "conPlay"); i++) {
        tick(*world, 1);
        dx::extension::tick(1.0f / 30.0f);
    }
    REQUIRE(objectProperty(player, "conPlay") == nullptr);
    CHECK(firstWindow->destroyed);
    CHECK(hudVisible->toText(hud->data + hudVisible->offset) == "True");
    dx::UObject* window = objectProperty(play, "datalink");
    REQUIRE(window != nullptr);
    CHECK_FALSE(window->destroyed);
    CHECK(objectProperty(hud, "infolink") == window);
    dx::UProperty* windowVisible = window->cls->findProperty(dx::Name("bIsVisible"));
    CHECK(windowVisible->toText(window->data + windowVisible->offset) == "True");
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("moving the player into a datalink trigger with SetLocation starts its transmission", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    tick(*world, 5);
    dx::UObject* hud = objectProperty(objectProperty(player, "rootWindow"), "hud");
    dx::UObject* trigger = findActor(*world, "DataLinkTrigger0");
    REQUIRE(trigger != nullptr);
    dx::Vector start = dx::actor::Location(player);
    dx::Vector inside = dx::actor::Location(trigger);
    inside.z = start.z;
    REQUIRE((start - inside).size2D() > dx::actor::CollisionRadius(trigger) + dx::actor::CollisionRadius(player));
    auto touches = [](dx::UObject* actor, dx::UObject* other) {
        for (int i = 0; i < 4; i++) {
            if (dx::actor::Touching(actor, i) == other) {
                return true;
            }
        }
        return false;
    };
    REQUIRE(world->farMoveActor(player, inside, true, false));
    CHECK_FALSE(touches(player, trigger));
    CHECK(objectProperty(player, "dataLinkPlay") == nullptr);
    REQUIRE(world->farMoveActor(player, start, false, true));
    dx::objects::setEditorMode(true);
    bool movedInEditor = world->farMoveActor(player, inside, false, false);
    dx::objects::setEditorMode(false);
    REQUIRE(movedInEditor);
    CHECK_FALSE(touches(player, trigger));
    CHECK(objectProperty(player, "dataLinkPlay") == nullptr);
    REQUIRE(world->farMoveActor(player, start, false, true));
    dx::vm::Call move(player, dx::Name("SetLocation"));
    move.v(inside);
    move.run();
    REQUIRE(*reinterpret_cast<uint32_t*>(move.returnValue()) != 0);
    CHECK(touches(player, trigger));
    CHECK(touches(trigger, player));
    dx::UObject* play = objectProperty(player, "dataLinkPlay");
    REQUIRE(play != nullptr);
    dx::UProperty* conName = dx::objects::findClass("ConSys.Conversation")->findProperty(dx::Name("conName"));
    dx::UObject* con = objectProperty(play, "con");
    REQUIRE(con != nullptr);
    CHECK(conName->toText(con->data + conName->offset) == "DL_StartGame");
    CHECK(objectProperty(hud, "infolink") == objectProperty(play, "datalink"));
    CHECK(objectProperty(play, "datalink") != nullptr);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("the belt slot of the item in hand draws its selection border", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    tick(*world, 5);
    dx::UObject* belt = objectProperty(objectProperty(objectProperty(player, "rootWindow"), "hud"), "belt");
    REQUIRE(belt != nullptr);
    auto slot = [&](int index) {
        return *reinterpret_cast<dx::UObject**>(belt->propertyAddress(dx::Name("objects"), index));
    };
    REQUIRE(objectProperty(slot(0), "item") == objectProperty(player, "KeyRing"));
    REQUIRE(objectProperty(player, "KeyRing") != nullptr);
    CHECK(dx::buttonwindow::bButtonPressed(slot(0)) == 0);
    dx::console::execute(player, "ActivateBelt 0");
    tick(*world, 60);
    REQUIRE(objectProperty(player, "inHand") == objectProperty(player, "KeyRing"));
    CHECK(dx::buttonwindow::bButtonPressed(slot(0)) != 0);
    CHECK(dx::buttonwindow::bButtonPressed(slot(1)) == 0);
    dx::console::execute(player, "PutInHand");
    tick(*world, 60);
    REQUIRE(objectProperty(player, "inHand") == nullptr);
    CHECK(dx::buttonwindow::bButtonPressed(slot(0)) == 0);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("the player's floor material comes from the mover being stood on", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::UObject* hatch = findActor(*world, "DeusExMover7");
    REQUIRE(hatch != nullptr);
    dx::console::execute(player, "teleport -6384 1420 -70 16384");
    tick(*world, 15);
    REQUIRE(dx::actor::Base(player) == hatch);
    dx::vm::Call call(player, dx::Name("GetFloorMaterial"));
    call.run();
    CHECK(reinterpret_cast<dx::Name*>(call.returnValue())->str() == "Mobile_Camp");
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("a walking player goes down a flight of stairs without jumping", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::console::execute(player, "teleport 1313 1290 130 49152");
    tick(*world, 20);
    REQUIRE(dx::actor::Location(player).z == Catch::Approx(127.8f).margin(1.0f));
    uint8_t walk = dx::console::keyFromName("Shift");
    uint8_t forward = dx::console::keyFromName("W");
    dx::console::key(walk, dx::IST_Press, 0);
    dx::console::key(forward, dx::IST_Press, 0);
    tick(*world, 90);
    dx::console::key(forward, dx::IST_Release, 0);
    dx::console::key(walk, dx::IST_Release, 0);
    CHECK(dx::pawn::bIsWalking(player) == 1);
    CHECK(dx::actor::Location(player).z == Catch::Approx(-96.1f).margin(1.0f));
    CHECK(dx::actor::Physics(player) == 1);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("a crouching player walks off the edge of a crate", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::console::execute(player, "teleport 688 -395 -50 0");
    tick(*world, 20);
    REQUIRE(dx::actor::Location(player).z == Catch::Approx(-51.6f).margin(1.0f));
    uint8_t duck = dx::console::keyFromName("X");
    uint8_t forward = dx::console::keyFromName("W");
    dx::console::key(duck, dx::IST_Press, 0);
    dx::console::key(forward, dx::IST_Press, 0);
    tick(*world, 60);
    dx::console::key(forward, dx::IST_Release, 0);
    CHECK(dx::pawn::bIsWalking(player) == 1);
    dx::Vector location = dx::actor::Location(player);
    dx::console::key(duck, dx::IST_Release, 0);
    CHECK(location.x > 760.0f);
    CHECK(location.z == Catch::Approx(-123.2f).margin(1.0f));
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("the pad browses consecutive save slots through the inline name editor", "[gamepad][data]")
{
    auto world = freshWorld("01_NYC_UNATCOIsland");
    tick(*world, 5);
    auto* root = dx::extension::rootWindow();
    auto* screen = dx::vm::Call(root, dx::Name("InvokeMenuScreen")).obj(dx::objects::findClass("DeusEx.MenuScreenSaveGame")).result<dx::UObject*>();
    REQUIRE(dx::script::isA(screen, "MenuScreenSaveGame"));
    // The save screen hides itself for two ticks to take the thumbnail.
    dx::vm::Call(screen, dx::Name("Tick")).f(0.033f).run();
    dx::vm::Call(screen, dx::Name("Tick")).f(0.033f).run();
    REQUIRE(dx::extension::topModal() == screen);
    auto* list = dx::cast<dx::extension::UListWindow>(objectProperty(screen, "lstGames"));
    REQUIRE(list);
    for (auto name : { "Slot one", "Slot two" }) {
        dx::vm::Call(list, dx::Name("AddRow")).s(std::string(name) + ";;;;99999").run();
    }
    int start = list->indexOf(dx::script::integer(screen, "editRowId"));
    REQUIRE(start >= 0);
    REQUIRE(start + 2 < static_cast<int>(list->rows.size()));
    dx::extension::setFocusWindow(objectProperty(screen, "editName"));
    for (int step : { 1, 2, 1 }) {
        int index = list->indexOf(dx::script::integer(screen, "editRowId"));
        REQUIRE(dx::gamepadmenu::adjust(index < start + step ? dx::IK_Down : dx::IK_Up));
        CHECK(dx::script::integer(screen, "editRowId") == list->rows[start + step].id);
    }
    REQUIRE(dx::gamepadmenu::press(dx::gamepad::A));
    CHECK(dx::extension::topModal() != screen);
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("Screen Resolution follows the window between windowed and fullscreen", "[data]")
{
    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    tick(*world, 5);
    std::string current = "1600x900";
    std::string available = "1280x720 1366x768 1600x900 1920x1080";
    auto savedCurrent = dx::console::currentResHook;
    auto savedAvailable = dx::console::resolutionsHook;
    dx::console::currentResHook = [&] { return current; };
    dx::console::resolutionsHook = [&] { return available; };
    dx::vm::Call(dx::extension::rootWindow(), dx::Name("InvokeMenuScreen")).obj(dx::objects::findClass("DeusEx.MenuScreenDisplay")).run();
    dx::UObject* screen = dx::extension::topModal();
    REQUIRE(dx::script::isA(screen, "MenuScreenDisplay"));
    dx::UObject* choice = nullptr;
    for (auto* child = dx::cast<dx::extension::UWindow>(objectProperty(screen, "winClient"))->firstChild(); child; child = child->nextSibling()) {
        if (dx::script::isA(child, "MenuChoice_Resolution")) {
            choice = child;
        }
    }
    REQUIRE(choice != nullptr);
    // The game adds this enum to Display at startup. Exercise its live instance:
    // moving the window must also remove entries left over from the faster screen.
    auto* frameClass = dx::objects::createSubclass(dx::objects::findClass("DeusEx.MenuUIChoiceEnum"), dx::Name("MenuChoice_FrameRateLimit"));
    auto* frameChoice = dx::vm::Call(objectProperty(screen, "winClient"), dx::Name("NewChild")).obj(frameClass).result<dx::UObject*>();
    REQUIRE(frameChoice);
    auto entry = [choice](int index) {
        return *reinterpret_cast<std::string*>(choice->propertyAddress(dx::Name("enumText"), index));
    };
    auto shown = [&] {
        return entry(dx::script::integer(choice, "currentValue"));
    };
    auto sensitive = [choice] {
        return dx::cast<dx::extension::UWindow>(dx::script::object(choice, "btnAction"))->sensitive();
    };
    dx::deusex::syncResolutionChoice(current, false, 720);
    CHECK(*dx::script::field<std::string>(frameChoice, "enumText", std::size(dx::settings::frameRates)) == "720");
    CHECK(shown() == "1600x900");
    CHECK(entry(3) == "1920x1080");
    CHECK(sensitive());

    // Borderless fullscreen: just the desktop's size, which can't be changed.
    current = "2560x1440";
    available = dx::resolutions::format({ { 2560, 1440 } });
    dx::deusex::syncResolutionChoice(current + " fullscreen", true, 100);
    CHECK(*dx::script::field<std::string>(frameChoice, "enumText", 4) == "100");
    CHECK(dx::script::field<std::string>(frameChoice, "enumText", 5)->empty());
    CHECK(shown() == "2560x1440");
    CHECK(entry(1).empty());
    CHECK_FALSE(sensitive());

    // Back in a window, the windowed sizes return with the window's own selected.
    current = "1600x900";
    available = "1280x720 1366x768 1600x900 1920x1080";
    dx::deusex::syncResolutionChoice(current, false, 60);
    CHECK(*dx::script::field<std::string>(frameChoice, "enumText", 2) == "60");
    CHECK(dx::script::field<std::string>(frameChoice, "enumText", 3)->empty());
    CHECK(shown() == "1600x900");
    CHECK(entry(3) == "1920x1080");
    CHECK(sensitive());

    dx::console::currentResHook = savedCurrent;
    dx::console::resolutionsHook = savedAvailable;
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("on a slider row the gamepad changes the value with left and right, stopping at the ends, and leaves with up and down", "[gamepad][data]")
{
    class LayoutCanvas : public dx::Canvas2D {
    public:
        float width() const override
        {
            return 1280;
        }

        float height() const override
        {
            return 720;
        }

        void setClip(float, float, float, float) override
        {
        }

        void clearClip() override
        {
        }

        void drawTile(dx::UTexture*, float, float, float, float, float, float, float, float, dx::LinearColor, dx::BlendStyle, bool) override
        {
        }

        void drawRect(float, float, float, float, dx::LinearColor, dx::BlendStyle) override
        {
        }

        void drawLine(float, float, float, float, dx::LinearColor) override
        {
        }
    } canvas;

    std::unique_ptr<dx::World> world = freshWorld("01_NYC_UNATCOIsland");
    tick(*world, 5);
    dx::vm::Call(dx::extension::rootWindow(), dx::Name("InvokeMenuScreen")).obj(dx::objects::findClass("DeusEx.MenuScreenSound")).run();
    REQUIRE(dx::script::isA(dx::extension::topModal(), "MenuScreenSound"));
    // As the controller sends arrows in a menu.
    auto press = [&](uint8_t key) {
        if (!dx::gamepadmenu::adjust(key)) {
            dx::extension::navigate(key);
        }
        tick(*world, 1);
        dx::extension::render(canvas);
    };
    auto row = [] {
        auto* focus = dx::cast<dx::extension::UWindow>(dx::extension::focusWindow());
        return focus && focus->parent() ? std::string(focus->parent()->cls->name.c_str()) : std::string();
    };
    tick(*world, 1);
    dx::extension::render(canvas);
    press(dx::IK_Down);
    REQUIRE(row() == "MenuChoice_MusicVolume");
    dx::UObject* music = dx::cast<dx::extension::UWindow>(dx::extension::focusWindow())->parent();
    dx::UObject* scale = dx::script::object(dx::script::object(music, "btnSlider"), "winSlider");
    auto tickPosition = [scale] {
        return dx::vm::Call(scale, dx::Name("GetTickPosition")).result<int32_t>();
    };
    int32_t ticks = dx::vm::Call(scale, dx::Name("GetNumTicks")).result<int32_t>();

    // Left and right change the volume without moving focus, and stop at the ends.
    dx::vm::Call(scale, dx::Name("SetTickPosition")).i(ticks - 2).run();
    press(dx::IK_Right);
    CHECK(tickPosition() == ticks - 1);
    press(dx::IK_Right);
    CHECK(tickPosition() == ticks - 1);
    CHECK(row() == "MenuChoice_MusicVolume");
    dx::vm::Call(scale, dx::Name("SetTickPosition")).i(1).run();
    press(dx::IK_Left);
    press(dx::IK_Left);
    CHECK(tickPosition() == 0);
    CHECK(row() == "MenuChoice_MusicVolume");

    // Up and down still leave the slider rows.
    press(dx::IK_Down);
    CHECK(row() == "MenuChoice_SoundVolume");
    press(dx::IK_Down);
    press(dx::IK_Down);
    press(dx::IK_Down);
    CHECK(row() == "MenuChoice_SampleRate");
    press(dx::IK_Up);
    CHECK(row() == "MenuChoice_EffectsChannels");
    releaseWorld(world, "01_NYC_UNATCOIsland");
}

#include "core/config.h"
#include "test_paths.h"
#include "core/math.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/aim_assist.h"
#include "deusex/game_state.h"
#include "deusex/known_logins.h"
#include "deusex/pad_inventory.h"
#include "deusex/reading.h"
#include "deusex/script.h"
#include "deusex/startup.h"
#include "engine/console.h"
#include "engine/gamepad.h"
#include "engine/input.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "extension/windows.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// Aim assist, the reading watcher and the pad inventory as they behave today.

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

    void unloadWorld(std::unique_ptr<dx::World>& world, const std::string& map)
    {
        dx::console::resetInput(world->player());
        dx::objects::unloadPackage(dx::Name(map));
        world.reset();
    }

    void tick(dx::World& world, int frames)
    {
        for (int i = 0; i < frames; i++) {
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

    dx::UObject* giveAndEquip(dx::World& world, dx::UObject* player, const char* className, const char* isA)
    {
        dx::console::execute(player, std::string("giveitem DeusEx.") + className);
        for (dx::UObject* entry : dx::script::inventory(player)) {
            if (dx::script::isA(entry, isA)) {
                dx::vm::Call(player, dx::Name("PutInHand")).obj(entry).run();
                tick(world, 15);
                return entry;
            }
        }
        return nullptr;
    }

    int32_t angleBetween(int32_t to, int32_t from)
    {
        return static_cast<int16_t>(static_cast<uint16_t>((to - from) & 65535));
    }
}

TEST_CASE("aim assist does nothing without a player in a world", "[gamepad]")
{
    CHECK(dx::aimassist::apply(nullptr, nullptr, 1.0f, 1.0f, 0.1f) == 1.0f);
}

TEST_CASE("aim assist slows turning over a hostile target, follows it and drifts toward it while aiming", "[gamepad][data]")
{
    std::unique_ptr<dx::World> world = loadWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::UObject* terrorist = findActor(*world, "Terrorist1");
    REQUIRE(terrorist != nullptr);
    // Where the terrorist can see, and be seen by, the player.
    dx::console::execute(player, "teleport -1900 -653 -95 32768");
    tick(*world, 2);
    auto aimAt = [&](int32_t yawOffset) {
        dx::Vector eye = dx::actor::Location(player);
        eye.z += dx::pawn::EyeHeight(player);
        dx::Vector chest = dx::actor::Location(terrorist);
        chest.z += 0.3f * dx::actor::CollisionHeight(terrorist);
        dx::Rotator wanted = dx::vectorRotation(chest - eye);
        dx::pawn::ViewRotation(player) = { wanted.pitch, (wanted.yaw + yawOffset) & 65535, 0 };
        return wanted;
    };

    SECTION("without a weapon in hand there is no assist")
    {
        aimAt(0);
        CHECK(dx::aimassist::apply(world.get(), player, 1.0f, 0.0f, 0.1f) == 1.0f);
    }
    REQUIRE(giveAndEquip(*world, player, "WeaponPistol", "WeaponPistol") != nullptr);
    SECTION("right on the target turning is halved")
    {
        aimAt(0);
        CHECK(dx::aimassist::apply(world.get(), player, 0.0f, 0.0f, 0.1f) == Catch::Approx(0.5f).margin(0.05f));
    }
    SECTION("a still stick never moves the view")
    {
        aimAt(300);
        dx::Rotator before = dx::pawn::ViewRotation(player);
        float factor = dx::aimassist::apply(world.get(), player, 0.0f, 0.0f, 0.1f);
        CHECK(factor > 0.5f);
        CHECK(factor < 1.0f);
        CHECK(dx::pawn::ViewRotation(player).yaw == before.yaw);
        CHECK(dx::pawn::ViewRotation(player).pitch == before.pitch);
    }
    SECTION("aiming near the target drifts the view toward it")
    {
        dx::Rotator wanted = aimAt(300);
        dx::aimassist::apply(world.get(), player, 1.0f, 0.0f, 0.1f);
        int32_t after = std::abs(angleBetween(wanted.yaw, dx::pawn::ViewRotation(player).yaw));
        CHECK(after < 300);
        CHECK(after > 0);
    }
    SECTION("a target well off the crosshair gets no assist")
    {
        aimAt(8192);
        dx::Rotator before = dx::pawn::ViewRotation(player);
        CHECK(dx::aimassist::apply(world.get(), player, 1.0f, 1.0f, 0.1f) == 1.0f);
        CHECK(dx::pawn::ViewRotation(player).yaw == before.yaw);
    }
    SECTION("aiming at the head turns as slowly as on the chest")
    {
        dx::Vector eye = dx::actor::Location(player);
        eye.z += dx::pawn::EyeHeight(player);
        dx::Vector head = dx::actor::Location(terrorist);
        head.z += 0.85f * dx::actor::CollisionHeight(terrorist);
        dx::Rotator wanted = dx::vectorRotation(head - eye);
        dx::pawn::ViewRotation(player) = { wanted.pitch, wanted.yaw & 65535, 0 };
        CHECK(dx::aimassist::apply(world.get(), player, 0.0f, 0.0f, 0.1f) == Catch::Approx(0.5f).margin(0.05f));
    }
    SECTION("the view follows a target moving across it, beyond the drift alone")
    {
        const float frame = 1.0f / 30.0f;
        aimAt(0);
        dx::aimassist::apply(world.get(), player, 1.0f, 0.0f, frame);
        // A frame of a run sideways to the view, which faces -X.
        dx::actor::Location(terrorist).y += 8.0f;
        dx::Rotator start = dx::pawn::ViewRotation(player);
        int32_t wantedYaw = (aimAt(0).yaw) & 65535;
        dx::pawn::ViewRotation(player) = start;
        int32_t before = std::abs(angleBetween(wantedYaw, start.yaw));
        REQUIRE(before > 50);
        dx::aimassist::apply(world.get(), player, 1.0f, 0.0f, frame);
        int32_t tracked = std::abs(angleBetween(wantedYaw, dx::pawn::ViewRotation(player).yaw));
        // The same frame with no memory of the last: the drift alone.
        dx::pawn::ViewRotation(player) = start;
        dx::aimassist::apply(nullptr, nullptr, 0.0f, 0.0f, frame);
        dx::aimassist::apply(world.get(), player, 1.0f, 0.0f, frame);
        int32_t drifted = std::abs(angleBetween(wantedYaw, dx::pawn::ViewRotation(player).yaw));
        CHECK(tracked < drifted);
        // Most of the way: trackingShare (0.6) near the middle of the target.
        CHECK(tracked < before * 6 / 10);
    }
    SECTION("a target keeps the assist a little past the cone that found it")
    {
        // The first offset, in steps of 20, where a fresh look finds nothing.
        int32_t edge = 0;
        for (int32_t offset = 20; offset < 4096 && edge == 0; offset += 20) {
            aimAt(offset);
            dx::aimassist::apply(nullptr, nullptr, 0.0f, 0.0f, 0.1f);
            if (dx::aimassist::apply(world.get(), player, 0.0f, 0.0f, 0.1f) == 1.0f) {
                edge = offset;
            }
        }
        REQUIRE(edge > 0);
        dx::aimassist::apply(nullptr, nullptr, 0.0f, 0.0f, 0.1f);
        aimAt(0);
        dx::aimassist::apply(world.get(), player, 0.0f, 0.0f, 0.1f);
        aimAt(edge);
        CHECK(dx::aimassist::apply(world.get(), player, 0.0f, 0.0f, 0.1f) < 1.0f);
    }
    unloadWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("reading a datacube and hearing an infolink are recorded as read", "[gamepad][data]")
{
    std::unique_ptr<dx::World> world = loadWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    tick(*world, 10);
    dx::gamestate::restore({});
    dx::reading::tick();
    CHECK(dx::knownlogins::knownAccounts("01_NYC_UNATCOIsland", "Keypad0").empty());

    SECTION("a datacube is read when its text opens")
    {
        dx::UObject* datacube = nullptr;
        for (dx::UObject* actor : world->actors) {
            if (actor && !actor->destroyed && dx::script::isA(actor, "InformationDevices") && dx::script::text(actor, "textTag") == "01_Datacube03") {
                datacube = actor;
            }
        }
        REQUIRE(datacube != nullptr);
        dx::vm::Call(datacube, dx::Name("Frob")).obj(player).obj(nullptr).run();
        tick(*world, 2);
        dx::reading::tick();
        CHECK(dx::gamestate::has("Read", "text:01_datacube03"));
        CHECK(dx::knownlogins::knownAccounts("01_NYC_UNATCOIsland", "Keypad0") == std::vector<std::string> { "" });
    }
    SECTION("an infolink is heard while it plays")
    {
        dx::vm::Call call(player, dx::Name("StartDataLinkTransmission"));
        call.s("DL_GateEntrance");
        call.skip();
        call.run();
        REQUIRE(*reinterpret_cast<uint32_t*>(call.returnValue()) != 0);
        tick(*world, 2);
        dx::reading::tick();
        CHECK(dx::gamestate::has("Read", "con:dl_gateentrance"));
    }
    dx::gamestate::restore({});
    unloadWorld(world, "01_NYC_UNATCOIsland");
}

TEST_CASE("the pad inventory stays out of the way off the inventory screen", "[gamepad]")
{
    CHECK_FALSE(dx::padinventory::busy());
    CHECK_FALSE(dx::padinventory::press(nullptr, dx::gamepad::A));
    CHECK_FALSE(dx::padinventory::steer(dx::IK_Down));
    CHECK_FALSE(dx::padinventory::keyPressed(dx::IK_Enter));
    CHECK_FALSE(dx::padinventory::buttonPressed(nullptr));
    CHECK_FALSE(dx::padinventory::mouseReleased(nullptr, dx::IK_LeftMouse));
    std::vector<dx::prompts::Prompt> prompts;
    dx::padinventory::addPrompts(nullptr, prompts);
    CHECK(prompts.empty());
    // Without the mod question, the held-item prompts.
    std::vector<dx::prompts::Prompt> held = dx::padinventory::busyPrompts();
    REQUIRE(held.size() == 3);
    CHECK(held[0].label == "Move");
    CHECK(held[1].label == "Place");
    CHECK(held[2].label == "Cancel");
}

TEST_CASE("an item picked up with the pad and put back returns to its cell", "[gamepad][data]")
{
    std::unique_ptr<dx::World> world = loadWorld("01_NYC_UNATCOIsland");
    dx::UObject* player = world->player();
    dx::console::execute(player, "giveitem DeusEx.Medkit");
    dx::vm::Call(player, dx::Name("ShowInventoryWindow")).run();
    tick(*world, 5);
    dx::UObject* screen = dx::extension::topModal();
    REQUIRE(dx::script::isA(screen, "PersonaScreenInventory"));
    auto* grid = dx::cast<dx::extension::UWindow>(dx::script::object(screen, "winItems"));
    REQUIRE(grid != nullptr);
    dx::UObject* medkit = nullptr;
    for (auto* button = grid->firstChild(); button; button = button->nextSibling()) {
        if (dx::script::isA(dx::script::object(button, "clientObject"), "MedKit")) {
            medkit = button;
        }
    }
    REQUIRE(medkit != nullptr);
    dx::UObject* item = dx::script::object(medkit, "clientObject");
    int homeX = dx::script::integer(item, "invPosX");
    int homeY = dx::script::integer(item, "invPosY");
    dx::extension::setFocusWindow(medkit);

    std::vector<dx::prompts::Prompt> prompts;
    dx::padinventory::addPrompts(screen, prompts);
    CHECK(std::any_of(prompts.begin(), prompts.end(), [](const auto& prompt) { return prompt.label == "Move"; }));
    CHECK(std::any_of(prompts.begin(), prompts.end(), [](const auto& prompt) { return prompt.label == "Sort"; }));

    REQUIRE(dx::padinventory::press(screen, dx::gamepad::button(dx::gamepad::Action::MoveItem)));
    REQUIRE(dx::padinventory::busy());
    // While held, the arrows steer the item rather than move focus.
    CHECK(dx::padinventory::steer(dx::IK_Right));
    CHECK(dx::padinventory::press(screen, dx::gamepad::button(dx::gamepad::Action::MenuBack)));
    CHECK_FALSE(dx::padinventory::busy());
    CHECK(dx::script::integer(item, "invPosX") == homeX);
    CHECK(dx::script::integer(item, "invPosY") == homeY);
    CHECK(dx::extension::topModal() == screen);
    dx::vm::Call(dx::extension::rootWindow(), dx::Name("PopWindow")).run();
    unloadWorld(world, "01_NYC_UNATCOIsland");
}

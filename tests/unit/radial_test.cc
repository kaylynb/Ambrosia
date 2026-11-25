#include "core/config.h"
#include "test_paths.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/quickslots.h"
#include "deusex/radial.h"
#include "deusex/script.h"
#include "deusex/startup.h"
#include "deusex/ui.h"
#include "engine/console.h"
#include "engine/resources.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

#include <utility>

namespace {
    class IconCanvas : public dx::Canvas2D {
    public:
        struct Tile {
            dx::UTexture* texture;
            dx::BlendStyle style;
            float u, v, width, height;
            dx::LinearColor color;
        };

        std::vector<Tile> tiles;

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

        void drawTile(dx::UTexture* texture, float, float, float, float, float u, float v, float width, float height, dx::LinearColor color, dx::BlendStyle style, bool) override
        {
            tiles.push_back({ texture, style, u, v, width, height, color });
        }

        void drawRect(float, float, float, float, dx::LinearColor, dx::BlendStyle) override
        {
        }

        void drawLine(float, float, float, float, dx::LinearColor) override
        {
        }
    };
}

TEST_CASE("translucent icon cropping ignores black without changing masked icon bounds", "[gamepad][data]")
{
    dx::engine::initialize(dx::test::gameData());
    std::vector<uint8_t> pixels(4 * 4 * 4, 0);
    for (size_t i = 0; i < 16; i++) {
        pixels[i * 4 + 3] = 255;
    }
    for (size_t i : { 5u, 10u }) {
        pixels[i * 4] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = 255;
    }
    dx::UTexture* icon = dx::ui::newTexture(4, 4, std::move(pixels));
    IconCanvas canvas;
    dx::radial::drawIcon(canvas, icon, 50, 50, 32, {});
    dx::radial::drawIcon(canvas, icon, 50, 50, 32, {}, dx::BlendStyle::Translucent);
    REQUIRE(canvas.tiles.size() == 2);
    CHECK(canvas.tiles[0].style == dx::BlendStyle::Masked);
    CHECK(canvas.tiles[0].u == 0);
    CHECK(canvas.tiles[0].v == 0);
    CHECK(canvas.tiles[0].width == 4);
    CHECK(canvas.tiles[0].height == 4);
    CHECK(canvas.tiles[1].style == dx::BlendStyle::Translucent);
    CHECK(canvas.tiles[1].u == 1);
    CHECK(canvas.tiles[1].v == 1);
    CHECK(canvas.tiles[1].width == 2);
    CHECK(canvas.tiles[1].height == 2);
}

TEST_CASE("icon cropping does not read past a mip shorter than its size", "[gamepad][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UTexture* icon = dx::ui::newTexture(4, 4, std::vector<uint8_t>(4 * 4 * 4, 255));
    icon->mips[0].data.resize(8);
    IconCanvas canvas;
    dx::radial::drawIcon(canvas, icon, 50, 50, 32, {});
    dx::radial::drawIcon(canvas, icon, 50, 50, 32, {}, dx::BlendStyle::Translucent);
    REQUIRE(canvas.tiles.size() == 2);
    for (const IconCanvas::Tile& tile : canvas.tiles) {
        CHECK(tile.u == 0);
        CHECK(tile.v == 0);
        CHECK(tile.width == 4);
        CHECK(tile.height == 4);
    }
}

TEST_CASE("augmentation icons use translucent blending in both radials and quick slots", "[gamepad][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UClass* hudClass = dx::objects::findClass("DeusEx.HUDActiveAug");
    REQUIRE(hudClass != nullptr);
    for (bool active : { false, true }) {
        dx::Color* hudColor = dx::script::field<dx::Color>(hudClass->defaultObject, active ? "colAugActive" : "colAugInactive");
        REQUIRE(hudColor != nullptr);
        dx::LinearColor color = dx::ui::augmentationColor(active);
        CHECK(color.r == hudColor->r / 255.0f);
        CHECK(color.g == hudColor->g / 255.0f);
        CHECK(color.b == hudColor->b / 255.0f);
    }
    dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        dx::console::execute(player, "loadout");
        auto augs = dx::radial::augEntries(player);
        auto belt = dx::radial::beltEntries(player);
        REQUIRE_FALSE(augs.empty());
        REQUIRE_FALSE(belt.empty());
        IconCanvas canvas;
        dx::radial::draw(canvas, augs, 0, "Augmentations", player);
        for (const auto& aug : augs) {
            REQUIRE(aug.icon != nullptr);
            bool found = false;
            for (const auto& tile : canvas.tiles) {
                if (tile.texture == aug.icon) {
                    found = true;
                    CHECK(tile.style == dx::BlendStyle::Translucent);
                    dx::LinearColor expected = dx::ui::augmentationColor(aug.active);
                    CHECK(tile.color.r == expected.r);
                    CHECK(tile.color.g == expected.g);
                    CHECK(tile.color.b == expected.b);
                }
            }
            CHECK(found);
        }
        canvas.tiles.clear();
        dx::radial::draw(canvas, belt, 0, "Belt", player);
        for (const auto& entry : belt) {
            for (const auto& tile : canvas.tiles) {
                if (tile.texture == entry.icon) {
                    CHECK(tile.style == dx::BlendStyle::Masked);
                }
            }
        }
        auto previous = dx::quickslots::assigned();
        dx::quickslots::assign(dx::quickslots::Up, augs[0].id);
        dx::quickslots::assign(dx::quickslots::Right, belt[0].id);
        dx::console::execute(player, augs[0].command);
        augs = dx::radial::augEntries(player);
        REQUIRE(augs[0].active);
        canvas.tiles.clear();
        dx::radial::draw(canvas, augs, 0, "Augmentations", player);
        for (const auto& tile : canvas.tiles) {
            if (tile.texture == augs[0].icon) {
                CHECK(tile.color.r == dx::ui::augmentationColor(true).r);
                CHECK(tile.color.g == dx::ui::augmentationColor(true).g);
                CHECK(tile.color.b == dx::ui::augmentationColor(true).b);
            }
        }
        canvas.tiles.clear();
        dx::quickslots::draw(canvas, player);
        int augDraws = 0;
        for (const auto& tile : canvas.tiles) {
            if (tile.texture == augs[0].icon) {
                augDraws++;
                CHECK(tile.style == dx::BlendStyle::Translucent);
                CHECK(tile.color.r == dx::ui::augmentationColor(true).r);
                CHECK(tile.color.g == dx::ui::augmentationColor(true).g);
                CHECK(tile.color.b == dx::ui::augmentationColor(true).b);
            }
            if (tile.texture == belt[0].icon) {
                CHECK(tile.style == dx::BlendStyle::Masked);
            }
        }
        CHECK(augDraws == 2); // Assigned slot and active augmentation row.
        for (int i = 0; i < dx::quickslots::Count; i++) {
            dx::quickslots::assign(static_cast<dx::quickslots::Slot>(i), previous[static_cast<size_t>(i)]);
        }
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("radial slices start at the top and run clockwise", "[gamepad]")
{
    CHECK(dx::radial::pick(0.0f, -1.0f, 10, 0.5f) == 0);
    CHECK(dx::radial::pick(1.0f, 0.0f, 4, 0.5f) == 1);
    CHECK(dx::radial::pick(0.0f, 1.0f, 4, 0.5f) == 2);
    CHECK(dx::radial::pick(-1.0f, 0.0f, 4, 0.5f) == 3);
    CHECK(dx::radial::pick(-0.1f, -1.0f, 10, 0.5f) == 0);
}

TEST_CASE("radial picks nothing inside the dead zone or with no slices", "[gamepad]")
{
    CHECK(dx::radial::pick(0.2f, 0.2f, 10, 0.5f) == -1);
    CHECK(dx::radial::pick(1.0f, 0.0f, 0, 0.5f) == -1);
}

TEST_CASE("an entry put on a quick slot leaves the slot it was on before", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::quickslots::assign(dx::quickslots::Up, "item:WeaponCrowbar");
    dx::quickslots::assign(dx::quickslots::Left, "item:WeaponCrowbar");
    CHECK(dx::quickslots::assigned()[dx::quickslots::Up].empty());
    CHECK(dx::quickslots::assigned()[dx::quickslots::Left] == "item:WeaponCrowbar");
    dx::quickslots::assign(dx::quickslots::Left, "");
}

TEST_CASE("quick slots put an item in hand and toggle an augmentation", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    // The belt lives in the HUD of the game's own root window, which other tests may have swapped.
    dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        dx::console::execute(player, "loadout");
        std::vector<dx::radial::Entry> belt = dx::radial::beltEntries(player);
        std::vector<dx::radial::Entry> augs = dx::radial::augEntries(player);
        REQUIRE(belt[3].id == "item:WeaponCrowbar");
        REQUIRE_FALSE(augs[0].active);
        dx::quickslots::assign(dx::quickslots::Up, belt[3].id);
        dx::quickslots::assign(dx::quickslots::Right, augs[0].id);
        dx::quickslots::use(player, dx::quickslots::Up);
        dx::UObject* pending = *reinterpret_cast<dx::UObject**>(player->propertyAddress(dx::Name("inHandPending")));
        REQUIRE(pending != nullptr);
        CHECK(pending->cls->name.str() == "WeaponCrowbar");
        dx::quickslots::use(player, dx::quickslots::Right);
        CHECK(dx::radial::augEntries(player)[0].active);
        dx::quickslots::assign(dx::quickslots::Up, "");
        dx::quickslots::assign(dx::quickslots::Right, "");
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("a depleted grenade quick slot survives load cleanup and works after restocking", "[gamepad][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    dx::World world;
    REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
    auto* player = world.player();
    dx::quickslots::assign(dx::quickslots::Left, "item:WeaponLAM");
    dx::quickslots::prune(player);
    CHECK(dx::quickslots::assigned()[dx::quickslots::Left] == "item:WeaponLAM");
    dx::quickslots::use(player, dx::quickslots::Left);
    CHECK_FALSE(dx::script::object(player, "inHandPending"));
    dx::console::execute(player, "loadout");
    dx::quickslots::use(player, dx::quickslots::Left);
    CHECK(dx::script::isA(dx::script::object(player, "inHandPending"), "WeaponLAM"));
    dx::quickslots::assign(dx::quickslots::Left, "");
}

TEST_CASE("the belt and augmentation wheels list the loadout's slots and hotkeyed augs", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    // The belt lives in the HUD of the game's own root window, which other tests may have swapped.
    dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        dx::console::execute(player, "loadout");
        SECTION("the ammo shortcut follows the weapon in hand and its spare ammunition")
        {
            auto& hand = *dx::script::field<dx::UObject*>(player, "inHand");
            hand = nullptr;
            CHECK_FALSE(dx::radial::canSwitchAmmo(player));
            for (dx::UObject* item : dx::script::inventory(player)) {
                if (dx::script::isA(item, "WeaponAssaultGun")) {
                    hand = item;
                    break;
                }
            }
            REQUIRE(hand != nullptr);
            auto* ammo = dx::vm::Call(player, dx::Name("FindInventoryType")).obj(dx::objects::findClass("DeusEx.Ammo20mm")).result<dx::UObject*>();
            if (!ammo) {
                ammo = world.spawn(dx::objects::findClass("DeusEx.Ammo20mm"), nullptr, {}, {}, {}, nullptr, false);
                REQUIRE(ammo != nullptr);
                dx::vm::Call(ammo, dx::Name("GiveTo")).obj(player).run();
            }
            auto& rounds = *dx::script::field<int32_t>(ammo, "AmmoAmount");
            rounds = 1;
            CHECK(dx::radial::canSwitchAmmo(player));
            rounds = 0;
            CHECK_FALSE(dx::radial::canSwitchAmmo(player));
        }
        std::vector<dx::radial::Entry> belt = dx::radial::beltEntries(player);
        REQUIRE(belt.size() == 10);
        CHECK(belt[0].command == "ActivateBelt 1");
        CHECK(belt[9].command == "ActivateBelt 0");
        for (const dx::radial::Entry& entry : belt) {
            INFO(entry.label);
            CHECK(entry.icon != nullptr);
            CHECK_FALSE(entry.label.empty());
        }
        std::vector<dx::radial::Entry> augs = dx::radial::augEntries(player);
        CHECK(augs.size() == 10);
        for (const dx::radial::Entry& entry : augs) {
            INFO(entry.label);
            CHECK(entry.toggle);
            CHECK(entry.command.rfind("ActivateAugmentation ", 0) == 0);
        }
        CHECK_FALSE(augs[0].active);
        dx::console::execute(player, augs[0].command);
        CHECK(dx::radial::augEntries(player)[0].active);
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

#include "core/config.h"
#include "test_paths.h"
#include "core/gc.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/gamepad_menus.h"
#include "deusex/pad_inventory.h"
#include "deusex/radial.h"
#include "deusex/script.h"
#include "deusex/startup.h"
#include "deusex/ui.h"
#include "engine/console.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "extension/windows.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
    class TileCanvas : public dx::Canvas2D {
    public:
        struct Tile {
            dx::UTexture* texture;
            float u, v, width, height;
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

        void drawTile(dx::UTexture* texture, float, float, float, float, float u, float v, float width, float height, dx::LinearColor, dx::BlendStyle, bool) override
        {
            tiles.push_back({ texture, u, v, width, height });
        }

        void drawRect(float, float, float, float, dx::LinearColor, dx::BlendStyle) override
        {
        }

        void drawLine(float, float, float, float, dx::LinearColor) override
        {
        }
    };

    // Frees what earlier tests handed to gc::releaseLater, such as the windows of a reset
    // window tree, and nothing else.
    void collectLeftovers()
    {
        dx::gc::Options none;
        none.candidate = [](const dx::UObject*) { return false; };
        dx::gc::collect(none);
    }

    bool alive(const dx::UObject* object, uint32_t handle)
    {
        return dx::objects::atHandle(handle) == object;
    }

    // The textures made at run time that a radial menu draws: its ring, slice, edges and glyphs.
    std::vector<dx::UTexture*> radialTextures()
    {
        std::vector<dx::radial::Entry> entries(4);
        for (size_t i = 0; i < entries.size(); i++) {
            entries[i].label = "Entry " + std::to_string(i);
            entries[i].command = "Entry";
            entries[i].id = "item:Entry" + std::to_string(i);
        }
        TileCanvas canvas;
        dx::radial::draw(canvas, entries, 1, "Test", nullptr);
        dx::UObject* transient = dx::objects::packageObject(dx::Name("Transient"));
        std::vector<dx::UTexture*> made;
        for (const TileCanvas::Tile& tile : canvas.tiles) {
            if (tile.texture && tile.texture->outer == transient && std::find(made.begin(), made.end(), tile.texture) == made.end()) {
                made.push_back(tile.texture);
            }
        }
        return made;
    }
}

TEST_CASE("textures the Deus Ex caches made survive a collection that may free them", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    collectLeftovers();
    std::vector<dx::UTexture*> cached = radialTextures();
    // The disc, the highlighted slice, two edge circles and at least one glyph.
    REQUIRE(cached.size() >= 5);
    dx::UTexture* loose = dx::ui::newTexture(2, 2, std::vector<uint8_t>(16, 255));
    std::vector<std::pair<dx::UTexture*, uint32_t>> handles;
    std::unordered_set<const dx::UObject*> candidates;
    for (dx::UTexture* texture : cached) {
        handles.emplace_back(texture, texture->handle);
        candidates.insert(texture);
    }
    uint32_t looseHandle = loose->handle;
    candidates.insert(loose);

    dx::gc::Options options;
    options.candidate = [&](const dx::UObject* object) { return candidates.count(object) != 0; };
    dx::gc::Result result = dx::gc::collect(options);
    CHECK(result.freed == 1);
    CHECK_FALSE(alive(loose, looseHandle));
    for (const auto& [texture, handle] : handles) {
        CHECK(alive(texture, handle));
    }
    // The caches still hand out the same textures.
    std::vector<dx::UTexture*> again = radialTextures();
    CHECK(again == cached);
}

TEST_CASE("an icon made in place of a freed one is measured afresh", "[data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    collectLeftovers();
    // Lit only in its middle four pixels, so it crops to them.
    for (int round = 0; round < 8; round++) {
        std::vector<uint8_t> pixels(4 * 4 * 4, 0);
        for (size_t i : { 5u, 6u, 9u, 10u }) {
            pixels[i * 4 + 3] = 255;
        }
        dx::UTexture* cropped = dx::ui::newTexture(4, 4, std::move(pixels));
        TileCanvas canvas;
        dx::radial::drawIcon(canvas, cropped, 50, 50, 32, {});
        dx::gc::release({ cropped });
        // Solid, so it is drawn whole, wherever in memory it lands.
        dx::UTexture* whole = dx::ui::newTexture(4, 4, std::vector<uint8_t>(4 * 4 * 4, 255));
        dx::radial::drawIcon(canvas, whole, 50, 50, 32, {});
        REQUIRE(canvas.tiles.size() == 2);
        CHECK(canvas.tiles[0].width == 2);
        CHECK(canvas.tiles[1].u == 0);
        CHECK(canvas.tiles[1].v == 0);
        CHECK(canvas.tiles[1].width == 4);
        CHECK(canvas.tiles[1].height == 4);
        dx::gc::release({ whole });
    }
}

TEST_CASE("an inventory item held with the pad is let go when its windows are freed", "[gamepad][data][gc]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        collectLeftovers();
        dx::console::execute(world.player(), "giveitem DeusEx.Medkit");
        dx::vm::Call(world.player(), dx::Name("ShowInventoryWindow")).run();
        for (int i = 0; i < 5; i++) {
            world.tick(1.0f / 30.0f);
        }
        dx::UObject* screen = dx::extension::topModal();
        REQUIRE(dx::script::isA(screen, "PersonaScreenInventory"));
        auto* grid = dx::cast<dx::extension::UWindow>(dx::script::object(screen, "winItems"));
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
        REQUIRE(dx::gamepadmenu::press(dx::gamepad::X));
        REQUIRE(dx::padinventory::busy());

        // The window tree goes as it does at a level change, and its windows are freed. The
        // held item must be forgotten without its windows being looked at again.
        uint32_t screenHandle = screen->handle;
        dx::extension::reset();
        collectLeftovers();
        REQUIRE_FALSE(alive(screen, screenHandle));
        CHECK_FALSE(dx::padinventory::busy());
        dx::console::resetInput(world.player());
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

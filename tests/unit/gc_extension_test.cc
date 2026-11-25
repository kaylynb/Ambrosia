#include "core/config.h"
#include "test_paths.h"
#include "core/gc.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/startup.h"
#include "engine/props_extension.h"
#include "extension/extension.h"
#include "extension/windows.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
    using dx::extension::UWindow;

    // A window tree under the plain Extension root window, torn down when the test ends.
    struct Tree {
        dx::UObject* root = nullptr;

        Tree()
        {
            dx::engine::initialize(dx::test::gameData());
            dx::extension::reset();
            std::string previous = dx::config::system().get("Engine.Engine", "Root").value_or("DeusEx.DeusExRootWindow");
            dx::test::atTestEnd([previous] {
                dx::config::system().set("Engine.Engine", "Root", previous);
            });
            dx::config::system().set("Engine.Engine", "Root", "Extension.RootWindow");
            dx::UClass* playerClass = dx::objects::findClass("Extension.PlayerPawnExt");
            dx::UObject* player = dx::objects::newObject(playerClass, transient(), dx::Name(), 0);
            dx::vm::Call init(player, dx::Name("InitRootWindow"));
            REQUIRE(init.valid());
            init.run();
            root = dx::extension::rootWindow();
            REQUIRE(root != nullptr);
            // Clears out windows earlier tests handed to the collector, so they are not
            // counted here.
            dx::gc::Options none;
            none.candidate = [](const dx::UObject*) { return false; };
            dx::gc::collect(none);
        }

        ~Tree()
        {
            dx::extension::reset();
        }

        static dx::UObject* transient()
        {
            return dx::objects::packageObject(dx::Name("Transient"));
        }

        template <typename T>
        T* child(dx::UObject* parent, const char* className)
        {
            dx::vm::Call call(parent, dx::Name("NewChild"));
            REQUIRE(call.valid());
            call.obj(dx::objects::findClass(className)).b(true);
            call.run();
            auto* made = dx::cast<T>(*reinterpret_cast<dx::UObject**>(call.returnValue()));
            REQUIRE(made != nullptr);
            return made;
        }
    };

    // Objects made by a test that only a test's collections may free.
    struct Candidates {
        std::unordered_set<const dx::UObject*> set;

        dx::UObject* texture()
        {
            dx::UObject* object = dx::objects::newObject(dx::objects::findClass("Engine.Texture"), Tree::transient(), dx::Name(), dx::RF_Transient);
            REQUIRE(object != nullptr);
            set.insert(object);
            return object;
        }

        dx::gc::Options options()
        {
            dx::gc::Options result;
            result.candidate = [this](const dx::UObject* object) { return set.count(object) != 0; };
            return result;
        }

        static bool alive(const dx::UObject* object, uint32_t handle)
        {
            return dx::objects::atHandle(handle) == object;
        }

        // Frees whatever survived, so later tests do not see it.
        ~Candidates()
        {
            std::vector<dx::UObject*> left;
            for (const dx::UObject* object : set) {
                if (std::find(dx::objects::all().begin(), dx::objects::all().end(), object) != dx::objects::all().end()) {
                    left.push_back(const_cast<dx::UObject*>(object));
                }
            }
            dx::gc::release(left);
        }
    };

    void destroy(dx::UObject* window)
    {
        dx::vm::Call call(window, dx::Name("Destroy"));
        REQUIRE(call.valid());
        call.run();
    }
}

TEST_CASE("textures and objects held only by window C++ state survive a collection", "[data][gc]")
{
    Tree tree;
    Candidates candidates;
    auto* button = tree.child<dx::extension::UButtonWindow>(tree.root, "Extension.ButtonWindow");
    auto* list = tree.child<dx::extension::UListWindow>(tree.root, "Extension.ListWindow");
    auto* computer = tree.child<dx::extension::UComputerWindow>(tree.root, "Extension.ComputerWindow");
    dx::UObject* buttonTexture = candidates.texture();
    dx::UObject* columnFont = candidates.texture();
    dx::UObject* clientObject = candidates.texture();
    dx::UObject* background = candidates.texture();
    dx::UObject* graphic = candidates.texture();
    dx::UObject* loose = candidates.texture();
    uint32_t looseHandle = loose->handle;

    button->buttonTextures[3] = buttonTexture;
    dx::extension::UListWindow::Column column;
    column.font = columnFont;
    list->columns.push_back(column);
    dx::extension::UListWindow::Row row;
    row.clientObject = clientObject;
    list->rows.push_back(row);
    computer->backgrounds[1] = background;
    dx::extension::UComputerWindow::Event event;
    event.type = dx::extension::UComputerWindow::EventGraphic;
    event.object = graphic;
    computer->queued.push_back(event);

    std::vector<std::pair<dx::UObject*, uint32_t>> held;
    for (dx::UObject* object : { buttonTexture, columnFont, clientObject, background, graphic }) {
        held.emplace_back(object, object->handle);
    }
    dx::gc::Result result = dx::gc::collect(candidates.options());
    CHECK(result.freed == 1);
    CHECK_FALSE(Candidates::alive(loose, looseHandle));
    candidates.set.erase(loose);
    for (auto [object, handle] : held) {
        CHECK(Candidates::alive(object, handle));
    }
    CHECK(button->buttonTextures[3] == buttonTexture);
    CHECK(list->rows.back().clientObject == clientObject);
    CHECK(computer->queued[0].object == graphic);
}

TEST_CASE("a GC's pushed states keep the fonts they copied", "[data][gc]")
{
    Tree tree;
    Candidates candidates;
    dx::UObject* font = candidates.texture();
    uint32_t fontHandle = font->handle;
    dx::extension::UGC* context = dx::extension::sharedGC();
    REQUIRE(context != nullptr);
    dx::UObject* previous = dx::gc::normalFont(context);
    // What PushGC saves: a copy of the GC's script data, taken while the font was set.
    dx::gc::normalFont(context) = font;
    dx::extension::UGC::Saved saved;
    saved.data.assign(context->data, context->data + context->cls->size);
    context->savedStates.push_back(saved);
    dx::gc::normalFont(context) = previous;
    size_t offset = static_cast<size_t>(context->propertyAddress(dx::Name("normalFont")) - context->data);

    dx::gc::collect(candidates.options());
    CHECK(Candidates::alive(font, fontHandle));
    REQUIRE(context->savedStates.size() == 1);
    CHECK(*reinterpret_cast<dx::UObject**>(context->savedStates[0].data.data() + offset) == font);
    context->savedStates.clear();
}

TEST_CASE("references to a released window become None", "[data][gc]")
{
    Tree tree;
    auto* tile = tree.child<dx::extension::UTileWindow>(tree.root, "Extension.TileWindow");
    auto* list = tree.child<dx::extension::UListWindow>(tree.root, "Extension.ListWindow");
    UWindow* inTile = tree.child<UWindow>(tile, "Extension.Window");
    UWindow* kept = tree.child<UWindow>(tile, "Extension.Window");
    auto* root = dx::cast<dx::extension::URootWindow>(tree.root);
    REQUIRE(root != nullptr);
    root->lastClickWindow = inTile;

    destroy(inTile);
    // Destroying forgets the window as the last one clicked.
    CHECK(root->lastClickWindow == nullptr);

    // Pointers to it that C++ state still has: a tile's last layout, a row's client object,
    // and the root's click memory.
    tile->holds.clear();
    dx::extension::UTileWindow::Hold hold;
    hold.window = inTile;
    tile->holds.push_back(hold);
    hold.window = kept;
    tile->holds.push_back(hold);
    dx::extension::UListWindow::Row row;
    row.clientObject = inTile;
    list->rows.push_back(row);
    root->lastClickWindow = inTile;
    uint32_t handle = inTile->handle;
    dx::gc::Result result = dx::gc::release({ inTile });
    CHECK(result.freed == 1);
    CHECK_FALSE(Candidates::alive(inTile, handle));
    CHECK(tile->holds[0].window == nullptr);
    CHECK(tile->holds[1].window == kept);
    CHECK(list->rows.back().clientObject == nullptr);
    CHECK(root->lastClickWindow == nullptr);
    CHECK(kept->parent() == tile);
    CHECK(dx::extension::rootWindow() == tree.root);
}

TEST_CASE("a destroyed window waits for the reset before it goes", "[data][gc]")
{
    Tree tree;
    UWindow* window = tree.child<UWindow>(tree.root, "Extension.Window");
    uint32_t handle = window->handle;
    destroy(window);
    // Nothing points at it but the list of destroyed windows the reset hands over.
    dx::gc::Options options;
    options.candidate = [window](const dx::UObject* object) { return object == window; };
    dx::gc::collect(options);
    CHECK(Candidates::alive(window, handle));
    dx::UObject* root = tree.root;
    uint32_t rootHandle = root->handle;
    dx::extension::reset();
    dx::gc::Options none;
    none.candidate = [](const dx::UObject*) { return false; };
    dx::gc::collect(none);
    CHECK_FALSE(Candidates::alive(window, handle));
    CHECK_FALSE(Candidates::alive(root, rootHandle));
}

TEST_CASE("every extension type reports its references once a window tree is made", "[data][gc]")
{
    Tree tree;
    tree.child<dx::extension::UButtonWindow>(tree.root, "Extension.ButtonWindow");
    tree.child<dx::extension::UListWindow>(tree.root, "Extension.ListWindow");
    tree.child<dx::extension::UTileWindow>(tree.root, "Extension.TileWindow");
    tree.child<dx::extension::UEditWindow>(tree.root, "Extension.EditWindow");
    REQUIRE(dx::extension::sharedGC() != nullptr);
    for (const std::string& type : dx::gc::unreportedTypes()) {
        INFO(type);
        CHECK(type.find("extension") == std::string::npos);
    }
}

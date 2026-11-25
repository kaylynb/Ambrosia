#include "core/config.h"
#include "test_paths.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/deusex.h"
#include "deusex/script.h"
#include "deusex/startup.h"
#include "deusex/text_entry.h"
#include "engine/console.h"
#include "engine/input.h"
#include "engine/props_engine.h"
#include "engine/props_extension.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "extension/windows.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
    void initializeEngine()
    {
        static bool initialized = false;
        if (!initialized) {
            dx::engine::initialize(dx::test::gameData());
            initialized = true;
        }
    }

    struct RecordedTile {
        dx::UTexture* texture = nullptr;
        float x = 0;
        float y = 0;
        float w = 0;
        float h = 0;
        float u = 0;
        float v = 0;
        float ul = 0;
        float vl = 0;
        dx::LinearColor color;
        dx::BlendStyle style = dx::BlendStyle::None;
    };

    struct RecordedRect {
        float x = 0;
        float y = 0;
        float w = 0;
        float h = 0;
    };

    class RecordingCanvas : public dx::Canvas2D {
    public:
        RecordingCanvas(float w, float h)
            : canvasWidth(w)
            , canvasHeight(h)
        {
        }

        float width() const override
        {
            return canvasWidth;
        }

        float height() const override
        {
            return canvasHeight;
        }

        void setClip(float, float, float, float) override
        {
        }

        void clearClip() override
        {
        }

        void drawTile(dx::UTexture* texture, float x, float y, float w, float h, float u, float v, float ul, float vl, dx::LinearColor color, dx::BlendStyle style, bool) override
        {
            tiles.push_back({ texture, x, y, w, h, u, v, ul, vl, color, style });
        }

        void drawRect(float x, float y, float w, float h, dx::LinearColor, dx::BlendStyle) override
        {
            rectangles.push_back({ x, y, w, h });
            rects++;
        }

        void drawLine(float, float, float, float, dx::LinearColor) override
        {
        }

        float canvasWidth;
        float canvasHeight;
        std::vector<RecordedTile> tiles;
        std::vector<RecordedRect> rectangles;
        int rects = 0;
    };

    dx::UObject* makeRoot(const char* rootClass = "Extension.RootWindow")
    {
        initializeEngine();
        dx::extension::reset();
        // Later tests expect the game's own root window class.
        std::string previous = dx::config::system().get("Engine.Engine", "Root").value_or("DeusEx.DeusExRootWindow");
        dx::test::atTestEnd([previous] {
            dx::config::system().set("Engine.Engine", "Root", previous);
        });
        dx::config::system().set("Engine.Engine", "Root", rootClass);
        dx::UClass* playerClass = dx::objects::findClass("Extension.PlayerPawnExt");
        dx::UObject* player = dx::objects::newObject(playerClass, dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
        dx::vm::Call init(player, dx::Name("InitRootWindow"));
        REQUIRE(init.valid());
        init.run();
        dx::UObject* root = dx::extension::rootWindow();
        REQUIRE(root != nullptr);
        return root;
    }

    dx::UObject* newChild(dx::UObject* parent, const char* className)
    {
        dx::vm::Call call(parent, dx::Name("NewChild"));
        REQUIRE(call.valid());
        call.obj(dx::objects::findClass(className)).b(true);
        call.run();
        return *reinterpret_cast<dx::UObject**>(call.returnValue());
    }

    dx::UObject* objectCall(dx::UObject* object, const char* function)
    {
        dx::vm::Call call(object, dx::Name(function));
        REQUIRE(call.valid());
        call.run();
        return *reinterpret_cast<dx::UObject**>(call.returnValue());
    }

    void voidCall(dx::UObject* object, const char* function)
    {
        dx::vm::Call call(object, dx::Name(function));
        REQUIRE(call.valid());
        call.run();
    }

    void setSize(dx::UObject* window, float w, float h)
    {
        dx::vm::Call call(window, dx::Name("SetSize"));
        call.f(w).f(h);
        call.run();
    }

    void setPos(dx::UObject* window, float x, float y)
    {
        dx::vm::Call call(window, dx::Name("SetPos"));
        call.f(x).f(y);
        call.run();
    }

    void setText(dx::UObject* window, const std::string& text)
    {
        dx::vm::Call call(window, dx::Name("SetText"));
        call.s(text);
        call.run();
    }

    void layout(float w, float h)
    {
        RecordingCanvas canvas(w, h);
        dx::extension::render(canvas);
    }

    bool boolResult(dx::vm::Call& call)
    {
        call.run();
        return *reinterpret_cast<uint32_t*>(call.returnValue()) != 0;
    }
}

TEST_CASE("a window that does not size itself prefers its current size", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* window = newChild(root, "Extension.Window");
    auto query = [window](float& w, float& h) {
        dx::vm::Call call(window, dx::Name("QueryPreferredSize"));
        call.f(0).f(0);
        call.run();
        w = *reinterpret_cast<float*>(call.parameter(0));
        h = *reinterpret_cast<float*>(call.parameter(1));
    };
    // XWindow::Init starts a window at 10x10, and with no background or
    // ParentRequestedPreferredSize of its own that is what it asks for.
    float w = 0;
    float h = 0;
    query(w, h);
    CHECK(w == 10.0f);
    CHECK(h == 10.0f);
    {
        dx::vm::Call call(window, dx::Name("QueryPreferredHeight"));
        call.f(50);
        call.run();
        CHECK(*reinterpret_cast<float*>(call.returnValue()) == 10.0f);
    }
    // A hardcoded size wins.
    setSize(window, 30, 20);
    query(w, h);
    CHECK(w == 30.0f);
    CHECK(h == 20.0f);
}

TEST_CASE("inventory stat values start beyond the fixed-width label column", "[data]")
{
    auto* row = newChild(makeRoot(), "DeusEx.PersonaInfoItemWindow");
    dx::vm::Call(row, dx::Name("SetItemInfo")).s("Clip").s("12 rounds").run();
    setSize(row, 200, 24);
    layout(640, 480);
    auto* label = dx::cast<dx::extension::UWindow>(dx::script::object(row, "winLabel"));
    auto* value = dx::cast<dx::extension::UWindow>(dx::script::object(row, "winText"));
    REQUIRE(label);
    REQUIRE(value);
    CHECK(label->width() == 70);
    CHECK(value->x() >= label->x() + label->width());
    CHECK(value->width() > 0);
}

TEST_CASE("windows are placed by alignment and first margin, and SetPos makes them left and top aligned", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    {
        dx::vm::Call call(modal, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    dx::UObject* window = newChild(modal, "Extension.Window");
    setSize(window, 100, 50);
    auto align = [window](uint8_t h, uint8_t v, std::vector<float> margins) {
        dx::vm::Call call(window, dx::Name("SetWindowAlignments"));
        call.byte(h).byte(v);
        for (float m : margins) {
            call.f(m);
        }
        call.run();
    };
    auto pos = [window]() {
        return dx::Vector { dx::window::X(window), dx::window::Y(window), 0 };
    };
    // Centered: x and y read the computed position.
    align(1, 1, {});
    layout(800, 600);
    float parentWidth = dx::window::Width(modal);
    float parentHeight = dx::window::Height(modal);
    CHECK(pos().x == static_cast<float>(static_cast<int>((parentWidth - 100) * 0.5f)));
    CHECK(pos().y == static_cast<float>(static_cast<int>((parentHeight - 50) * 0.5f)));

    // Dragging a centered menu: SetPos(x + dx, y + dy) moves it by exactly that much.
    dx::Vector before = pos();
    setPos(window, before.x + 7, before.y - 5);
    layout(800, 600);
    CHECK(pos().x == before.x + 7);
    CHECK(pos().y == before.y - 5);
    CHECK(dx::window::winHAlign(window) == 0);
    CHECK(dx::window::winVAlign(window) == 0);

    // Right and bottom count in from the far edge by the first margins.
    align(2, 2, { 12, 8 });
    layout(800, 600);
    CHECK(pos().x == parentWidth - 100 - 12);
    CHECK(pos().y == parentHeight - 50 - 8);

    // Left-out margins are 0, not what SetPos left behind.
    setPos(window, 40, 30);
    align(0, 0, {});
    layout(800, 600);
    CHECK(pos().x == 0.0f);
    CHECK(pos().y == 0.0f);
}

TEST_CASE("NewChild links windows in order and Raise, Lower and Destroy relink siblings", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* a = newChild(root, "Extension.Window");
    dx::UObject* b = newChild(root, "Extension.Window");
    dx::UObject* c = newChild(root, "Extension.Window");
    REQUIRE(a != nullptr);
    CHECK(objectCall(root, "GetBottomChild") == a);
    CHECK(objectCall(root, "GetTopChild") == c);
    CHECK(objectCall(b, "GetParent") == root);
    CHECK(objectCall(b, "GetRootWindow") == root);

    voidCall(b, "Raise");
    CHECK(objectCall(root, "GetTopChild") == b);
    CHECK(objectCall(a, "GetHigherSibling") == c);

    voidCall(c, "Lower");
    CHECK(objectCall(root, "GetBottomChild") == c);
    CHECK(objectCall(c, "GetHigherSibling") == a);

    voidCall(b, "Destroy");
    CHECK(objectCall(root, "GetTopChild") == a);
    CHECK(objectCall(a, "GetLowerSibling") == c);
    CHECK(objectCall(a, "GetHigherSibling") == nullptr);
}

TEST_CASE("Horizontal tile window lays out children with margins and spacing", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* tile = newChild(root, "Extension.TileWindow");
    {
        dx::vm::Call call(tile, dx::Name("SetMargins"));
        call.f(2).f(3);
        call.run();
    }
    {
        dx::vm::Call call(tile, dx::Name("SetMinorSpacing"));
        call.f(5);
        call.run();
    }
    dx::UObject* first = newChild(tile, "Extension.Window");
    dx::UObject* second = newChild(tile, "Extension.Window");
    dx::UObject* third = newChild(tile, "Extension.Window");
    setSize(first, 10, 20);
    setSize(second, 30, 15);
    setSize(third, 20, 25);
    layout(800, 600);

    CHECK(dx::window::Width(tile) == 74.0f);
    CHECK(dx::window::Height(tile) == 31.0f);
    CHECK(dx::window::X(first) == 2.0f);
    CHECK(dx::window::X(second) == 17.0f);
    CHECK(dx::window::X(third) == 52.0f);
    CHECK(dx::window::Y(first) == 3.0f);
    CHECK(dx::window::Y(third) == 3.0f);
    CHECK(dx::window::Width(second) == 30.0f);
    CHECK(dx::window::Height(third) == 25.0f);
}

TEST_CASE("Text window preferred size follows TechMedium metrics and wraps words", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* text = newChild(root, "Extension.TextWindow");
    setText(text, "Hello World");
    {
        dx::vm::Call call(text, dx::Name("QueryPreferredWidth"));
        call.f(0);
        call.run();
        CHECK(*reinterpret_cast<float*>(call.returnValue()) == 69.0f);
    }
    {
        dx::vm::Call call(text, dx::Name("SetTextMargins"));
        call.f(4).f(2);
        call.run();
    }
    {
        dx::vm::Call call(text, dx::Name("QueryPreferredSize"));
        call.f(0).f(0);
        call.run();
        CHECK(*reinterpret_cast<float*>(call.parameter(0)) == 71.0f);
        CHECK(*reinterpret_cast<float*>(call.parameter(1)) == 15.0f);
    }
    {
        dx::vm::Call call(text, dx::Name("SetWordWrap"));
        call.b(true);
        call.run();
    }
    {
        dx::vm::Call call(text, dx::Name("QueryPreferredHeight"));
        call.f(40);
        call.run();
        CHECK(*reinterpret_cast<float*>(call.returnValue()) == 26.0f);
    }
}

TEST_CASE("Window background is drawn at its position scaled by the virtual resolution", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* window = newChild(root, "Extension.Window");
    dx::UObject* texture = dx::objects::loadObject("Engine.DefaultTexture", nullptr);
    REQUIRE(dx::cast<dx::UTexture>(texture) != nullptr);
    {
        dx::vm::Call call(window, dx::Name("SetBackground"));
        call.obj(texture);
        call.run();
    }
    setPos(window, 10, 20);
    setSize(window, 64, 32);

    RecordingCanvas small(800, 600);
    dx::extension::render(small);
    REQUIRE(small.tiles.size() == 1);
    CHECK(small.tiles[0].texture == texture);
    CHECK(small.tiles[0].x == 10.0f);
    CHECK(small.tiles[0].y == 20.0f);
    CHECK(small.tiles[0].w == 64.0f);
    CHECK(small.tiles[0].h == 32.0f);
    CHECK(small.tiles[0].ul == 64.0f);
    CHECK(small.tiles[0].vl == 32.0f);
    CHECK(small.tiles[0].style == dx::BlendStyle::Normal);

    RecordingCanvas large(1600, 1200);
    dx::extension::render(large);
    REQUIRE(large.tiles.size() == 1);
    CHECK(large.tiles[0].x == 20.0f);
    CHECK(large.tiles[0].y == 40.0f);
    CHECK(large.tiles[0].w == 128.0f);
    CHECK(large.tiles[0].h == 64.0f);
    CHECK(large.tiles[0].ul == 64.0f);
}

TEST_CASE("Window background at a half pixel position is drawn on whole pixels", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* window = newChild(root, "Extension.Window");
    dx::UObject* texture = dx::objects::loadObject("Engine.DefaultTexture", nullptr);
    REQUIRE(dx::cast<dx::UTexture>(texture) != nullptr);
    {
        dx::vm::Call call(window, dx::Name("SetBackground"));
        call.obj(texture);
        call.run();
    }
    setPos(window, 392.5f, 292.5f);
    setSize(window, 16, 16);

    RecordingCanvas canvas(800, 600);
    dx::extension::render(canvas);
    REQUIRE(canvas.tiles.size() == 1);
    CHECK(canvas.tiles[0].x == 392.0f);
    CHECK(canvas.tiles[0].y == 292.0f);
    CHECK(canvas.tiles[0].w == 16.0f);
    CHECK(canvas.tiles[0].h == 16.0f);
    CHECK(canvas.tiles[0].u == 0.0f);
    CHECK(canvas.tiles[0].v == 0.0f);
    CHECK(canvas.tiles[0].ul == 16.0f);
    CHECK(canvas.tiles[0].vl == 16.0f);
}

TEST_CASE("Full width window background covers an ultrawide screen while children stay in the 16:9 area", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* full = newChild(root, "Extension.Window");
    {
        dx::vm::Call call(full, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    dx::UObject* texture = dx::objects::loadObject("Engine.DefaultTexture", nullptr);
    REQUIRE(dx::cast<dx::UTexture>(texture) != nullptr);
    {
        dx::vm::Call call(full, dx::Name("SetBackground"));
        call.obj(texture);
        call.run();
    }
    dx::UObject* child = newChild(full, "Extension.Window");
    {
        dx::vm::Call call(child, dx::Name("SetBackground"));
        call.obj(texture);
        call.run();
    }
    setPos(child, 10, 20);
    setSize(child, 64, 32);

    RecordingCanvas canvas(3360, 1440);
    dx::extension::render(canvas);
    REQUIRE(canvas.tiles.size() == 2);
    CHECK(canvas.tiles[0].x == 0.0f);
    CHECK(canvas.tiles[0].y == 0.0f);
    CHECK(canvas.tiles[0].w == 3360.0f);
    CHECK(canvas.tiles[0].h == 1440.0f);
    CHECK(canvas.tiles[0].u == -200.0f);
    CHECK(canvas.tiles[0].ul == 1680.0f);
    CHECK(canvas.tiles[1].x == 420.0f);
    CHECK(canvas.tiles[1].y == 40.0f);
    CHECK(canvas.tiles[1].w == 128.0f);
}

TEST_CASE("Text window draws one glyph tile per character from the font texture", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* text = newChild(root, "Extension.TextWindow");
    setText(text, "He");
    setPos(text, 100, 50);
    setSize(text, 50, 20);
    RecordingCanvas canvas(800, 600);
    dx::extension::render(canvas);
    REQUIRE(canvas.tiles.size() == 2);
    CHECK(canvas.tiles[0].x == 118.0f);
    CHECK(canvas.tiles[0].y == 54.0f);
    CHECK(canvas.tiles[0].w == 7.0f);
    CHECK(canvas.tiles[0].h == 10.0f);
    CHECK(canvas.tiles[0].u == 10.0f);
    CHECK(canvas.tiles[0].v == 12.0f);
    CHECK(canvas.tiles[1].x == 125.0f);
    CHECK(canvas.tiles[1].u == 213.0f);
    CHECK(canvas.tiles[1].v == 12.0f);
    CHECK(canvas.tiles[0].style == dx::BlendStyle::Masked);
    CHECK(canvas.tiles[0].color.r == 0.0f);
    CHECK(canvas.tiles[0].color.g == 1.0f);
}

TEST_CASE("Left aligned text in a window at a half pixel position keeps the first column of its first glyph", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* text = newChild(root, "Extension.TextWindow");
    setText(text, "He");
    {
        dx::vm::Call call(text, dx::Name("SetTextAlignments"));
        call.byte(0).byte(0);
        call.run();
    }
    setPos(text, 100.5f, 50.5f);
    setSize(text, 50, 20);
    RecordingCanvas canvas(800, 600);
    dx::extension::render(canvas);
    REQUIRE(canvas.tiles.size() == 2);
    CHECK(canvas.tiles[0].x == 103.0f);
    CHECK(canvas.tiles[0].y == 53.0f);
    CHECK(canvas.tiles[0].w == 7.0f);
    CHECK(canvas.tiles[0].h == 10.0f);
    CHECK(canvas.tiles[0].u == 10.0f);
    CHECK(canvas.tiles[0].v == 12.0f);
    CHECK(canvas.tiles[1].x == 110.0f);
    CHECK(canvas.tiles[1].u == 213.0f);
}

TEST_CASE("Clicking a checkbox inside a modal window toggles it", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    {
        dx::vm::Call call(modal, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    dx::UObject* box = newChild(modal, "Extension.CheckboxWindow");
    setPos(box, 100, 100);
    setSize(box, 40, 20);
    layout(800, 600);
    CHECK(dx::extension::wantsMouse());

    dx::extension::mouseMove(300, 300);
    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    {
        dx::vm::Call call(box, dx::Name("GetToggle"));
        CHECK_FALSE(boolResult(call));
    }

    dx::extension::mouseMove(110, 110);
    CHECK(dx::extension::mouseButton(dx::IK_LeftMouse, true));
    CHECK(dx::extension::mouseButton(dx::IK_LeftMouse, false));
    {
        dx::vm::Call call(box, dx::Name("GetToggle"));
        CHECK(boolResult(call));
    }
}

TEST_CASE("Right clicking a menu choice button cycles to the previous value", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    {
        dx::vm::Call call(modal, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    dx::UObject* choice = newChild(modal, "DeusEx.MenuChoice_LowMedHigh");
    REQUIRE(choice != nullptr);
    setPos(choice, 100, 100);
    {
        dx::vm::Call call(choice, dx::Name("SetValue"));
        call.i(1);
        call.run();
    }
    layout(800, 600);
    dx::extension::mouseMove(105, 105);
    dx::extension::mouseButton(dx::IK_RightMouse, true);
    dx::extension::mouseButton(dx::IK_RightMouse, false);
    CHECK(*reinterpret_cast<int32_t*>(choice->propertyAddress(dx::Name("currentValue"))) == 0);
    dx::extension::mouseButton(dx::IK_RightMouse, true);
    dx::extension::mouseButton(dx::IK_RightMouse, false);
    CHECK(*reinterpret_cast<int32_t*>(choice->propertyAddress(dx::Name("currentValue"))) == 2);
    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    CHECK(*reinterpret_cast<int32_t*>(choice->propertyAddress(dx::Name("currentValue"))) == 0);
}

TEST_CASE("A toggled toggle window stays pressed through clicks and radio group changes", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    {
        dx::vm::Call call(modal, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    dx::UObject* toggle = newChild(modal, "Extension.ToggleWindow");
    setPos(toggle, 100, 100);
    setSize(toggle, 40, 20);
    layout(800, 600);
    auto setToggle = [](dx::UObject* window, bool value) {
        dx::vm::Call call(window, dx::Name("SetToggle"));
        call.b(value);
        call.run();
    };
    CHECK(dx::buttonwindow::bButtonPressed(toggle) == 0);
    setToggle(toggle, true);
    CHECK(dx::buttonwindow::bButtonPressed(toggle) != 0);
    setToggle(toggle, false);
    CHECK(dx::buttonwindow::bButtonPressed(toggle) == 0);

    dx::extension::mouseMove(110, 110);
    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    CHECK(dx::buttonwindow::bButtonPressed(toggle) != 0);

    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    dx::extension::mouseMove(300, 300);
    CHECK(dx::buttonwindow::bButtonPressed(toggle) != 0);
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    CHECK(dx::buttonwindow::bButtonPressed(toggle) != 0);

    dx::extension::mouseMove(110, 110);
    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    CHECK(dx::buttonwindow::bButtonPressed(toggle) == 0);

    dx::UObject* radio = newChild(modal, "Extension.RadioBoxWindow");
    dx::UObject* first = newChild(radio, "Extension.ToggleWindow");
    dx::UObject* second = newChild(radio, "Extension.ToggleWindow");
    setToggle(first, true);
    CHECK(dx::buttonwindow::bButtonPressed(first) != 0);
    setToggle(second, true);
    CHECK(dx::buttonwindow::bButtonPressed(first) == 0);
    CHECK(dx::buttonwindow::bButtonPressed(second) != 0);
}

TEST_CASE("FlagBase stores typed flags and deletes expired ones", "[data]")
{
    initializeEngine();
    dx::UObject* base = dx::objects::newObject(dx::objects::findClass("Extension.FlagBase"), dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
    {
        dx::vm::Call call(base, dx::Name("SetBool"));
        call.n(dx::Name("Ford")).b(true).b(true);
        CHECK(boolResult(call));
    }
    {
        dx::vm::Call call(base, dx::Name("GetBool"));
        call.n(dx::Name("Ford"));
        CHECK(boolResult(call));
    }
    {
        dx::vm::Call call(base, dx::Name("GetBool"));
        call.n(dx::Name("Missing"));
        CHECK_FALSE(boolResult(call));
    }
    {
        dx::vm::Call call(base, dx::Name("SetInt"));
        call.n(dx::Name("Count")).i(5).b(false);
        CHECK_FALSE(boolResult(call));
    }
    {
        dx::vm::Call call(base, dx::Name("SetInt"));
        call.n(dx::Name("Count")).i(7).b(true);
        CHECK(boolResult(call));
    }
    {
        dx::vm::Call call(base, dx::Name("SetFloat"));
        call.n(dx::Name("Count")).f(1.5f).b(true);
        CHECK(boolResult(call));
    }
    {
        dx::vm::Call call(base, dx::Name("GetInt"));
        call.n(dx::Name("Count"));
        call.run();
        CHECK(*reinterpret_cast<int32_t*>(call.returnValue()) == 7);
    }
    {
        dx::vm::Call call(base, dx::Name("GetFloat"));
        call.n(dx::Name("Count"));
        call.run();
        CHECK(*reinterpret_cast<float*>(call.returnValue()) == 1.5f);
    }
    {
        dx::vm::Call call(base, dx::Name("SetBool"));
        call.n(dx::Name("Old")).b(true).b(true).i(3);
        CHECK(boolResult(call));
    }
    {
        dx::vm::Call call(base, dx::Name("SetBool"));
        call.n(dx::Name("New")).b(true).b(true).i(5);
        CHECK(boolResult(call));
    }
    {
        dx::vm::Call call(base, dx::Name("DeleteExpiredFlags"));
        call.i(4);
        call.run();
    }
    auto exists = [base](const char* name) {
        dx::vm::Call call(base, dx::Name("CheckFlag"));
        call.n(dx::Name(name)).byte(0);
        call.run();
        return *reinterpret_cast<uint32_t*>(call.returnValue()) != 0;
    };
    CHECK_FALSE(exists("Old"));
    CHECK(exists("New"));
    CHECK(exists("Ford"));

    int32_t iterator = 0;
    {
        dx::vm::Call call(base, dx::Name("CreateIterator"));
        call.byte(0);
        call.run();
        iterator = *reinterpret_cast<int32_t*>(call.returnValue());
    }
    int count = 0;
    while (true) {
        dx::vm::Call call(base, dx::Name("GetNextFlagName"));
        call.i(iterator).n(dx::Name());
        if (!boolResult(call)) {
            break;
        }
        count++;
    }
    CHECK(count == 2);
}

TEST_CASE("List window sorts rows by a float column", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* list = newChild(root, "Extension.ListWindow");
    {
        dx::vm::Call call(list, dx::Name("SetNumColumns"));
        call.i(2);
        call.run();
    }
    {
        dx::vm::Call call(list, dx::Name("SetColumnType"));
        call.i(1).byte(1);
        call.run();
    }
    std::vector<int32_t> ids;
    for (const char* row : { "b;2", "a;10", "c;1" }) {
        dx::vm::Call call(list, dx::Name("AddRow"));
        call.s(row);
        call.run();
        ids.push_back(*reinterpret_cast<int32_t*>(call.returnValue()));
    }
    {
        dx::vm::Call call(list, dx::Name("SetSortColumn"));
        call.i(1);
        call.run();
    }
    voidCall(list, "Sort");
    auto rowAt = [list](int32_t index) {
        dx::vm::Call call(list, dx::Name("IndexToRowId"));
        call.i(index);
        call.run();
        return *reinterpret_cast<int32_t*>(call.returnValue());
    };
    CHECK(rowAt(0) == ids[2]);
    CHECK(rowAt(1) == ids[0]);
    CHECK(rowAt(2) == ids[1]);
    {
        dx::vm::Call call(list, dx::Name("GetField"));
        call.i(ids[1]).i(0);
        call.run();
        CHECK(*reinterpret_cast<std::string*>(call.returnValue()) == "a");
    }
}

TEST_CASE("Text parser tokenizes a DeusExText book", "[data]")
{
    initializeEngine();
    dx::UObject* parser = dx::objects::newObject(dx::objects::findClass("DeusExText.DeusExTextParser"), dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
    {
        dx::vm::Call call(parser, dx::Name("OpenText"));
        call.n(dx::Name("00_Book01"));
        REQUIRE(boolResult(call));
    }

    struct Token {
        uint8_t tag;
        std::string text;
    };

    std::vector<Token> tokens;
    for (int i = 0; i < 11; i++) {
        dx::vm::Call process(parser, dx::Name("ProcessText"));
        REQUIRE(boolResult(process));
        dx::vm::Call tag(parser, dx::Name("GetTag"));
        tag.run();
        dx::vm::Call text(parser, dx::Name("GetText"));
        text.run();
        tokens.push_back({ *tag.returnValue(), *reinterpret_cast<std::string*>(text.returnValue()) });
    }
    CHECK(tokens[0].tag == 15);
    CHECK(tokens[1].tag == 12);
    CHECK(tokens[2].tag == 19);
    CHECK(tokens[3].tag == 0);
    CHECK(tokens[3].text == "UNATCO TRAINING MANUAL");
    CHECK(tokens[4].tag == 20);
    CHECK(tokens[5].tag == 0);
    CHECK(tokens[5].text == "  ");
    CHECK(tokens[6].tag == 18);
    CHECK(tokens[7].text == "  ");
    CHECK(tokens[8].tag == 18);
    CHECK(tokens[9].tag == 12);
    CHECK(tokens[10].tag == 0);
    CHECK(tokens[10].text == "Section 3C: Multitools  ");
}

TEST_CASE("ConEvent sound length divides the data size by the first frame bitrate", "[data]")
{
    initializeEngine();
    dx::UObject* soundObject = dx::objects::newObject(dx::objects::findClass("Engine.Sound"), dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
    dx::USound* sound = dx::cast<dx::USound>(soundObject);
    REQUIRE(sound != nullptr);
    sound->bytes.assign(3 * 130, 0);
    for (size_t frame = 0; frame < 3; frame++) {
        sound->bytes[frame * 130 + 0] = 0xff;
        sound->bytes[frame * 130 + 1] = 0xfb;
        sound->bytes[frame * 130 + 2] = 0x20;
        sound->bytes[frame * 130 + 3] = 0xc4;
    }
    dx::UObject* event = dx::objects::newObject(dx::objects::findClass("ConSys.ConEvent"), dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
    dx::vm::Call call(event, dx::Name("GetSoundLength"));
    REQUIRE(call.valid());
    call.obj(sound);
    call.run();
    CHECK(*reinterpret_cast<float*>(call.returnValue()) == Catch::Approx(390.0 * 8.0 / 40000.0).epsilon(1e-5));
}

TEST_CASE("Text window wraps words without an explicit SetWordWrap", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* text = newChild(root, "Extension.TextWindow");
    setText(text, "Hello World");
    dx::vm::Call call(text, dx::Name("QueryPreferredHeight"));
    call.f(40);
    call.run();
    CHECK(*reinterpret_cast<float*>(call.returnValue()) == 28.0f);
}

TEST_CASE("Computer window prefers the size of its text grid", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* computer = newChild(root, "Extension.ComputerWindow");
    {
        dx::vm::Call call(computer, dx::Name("SetTextSize"));
        call.i(10).i(3);
        call.run();
    }
    {
        dx::vm::Call call(computer, dx::Name("SetTextFont"));
        call.obj(nullptr).i(6).i(10).skip();
        call.run();
    }
    dx::vm::Call call(computer, dx::Name("QueryPreferredSize"));
    call.f(0).f(0);
    call.run();
    CHECK(*reinterpret_cast<float*>(call.parameter(0)) == 60.0f);
    CHECK(*reinterpret_cast<float*>(call.parameter(1)) == 30.0f);
}

TEST_CASE("Computer window moves a word that does not fit to the next row", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* computer = newChild(root, "Extension.ComputerWindow");
    {
        dx::vm::Call call(computer, dx::Name("SetTextSize"));
        call.i(10).i(3);
        call.run();
    }
    {
        dx::vm::Call call(computer, dx::Name("Print"));
        call.s("hello wonderful world").b(false);
        call.run();
    }
    dx::extension::tick(1.0f);
    dx::extension::tick(1.0f);
    dx::extension::UComputerWindow* window = dx::cast<dx::extension::UComputerWindow>(computer);
    REQUIRE(window != nullptr);
    std::string rows(30, ' ');
    for (const dx::extension::UComputerWindow::Event& element : window->display) {
        REQUIRE(element.row >= 0);
        REQUIRE(element.row < 3);
        REQUIRE(element.col >= 0);
        REQUIRE(element.col < 10);
        rows[static_cast<size_t>(element.row * 10 + element.col)] = element.ch;
    }
    CHECK(rows.substr(0, 10) == "hello     ");
    CHECK(rows.substr(10, 10) == "wonderful ");
    CHECK(rows.substr(20, 10) == "world     ");
}

TEST_CASE("Computer window characters fade in over the fade speed", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* computer = newChild(root, "Extension.ComputerWindow");
    {
        dx::vm::Call call(computer, dx::Name("SetFadeSpeed"));
        call.f(1.0f);
        call.run();
    }
    {
        dx::vm::Call call(computer, dx::Name("Print"));
        call.s("a").b(false);
        call.run();
    }
    dx::extension::tick(0.5f);
    dx::extension::UComputerWindow* window = dx::cast<dx::extension::UComputerWindow>(computer);
    REQUIRE(window != nullptr);
    CHECK(window->display.empty());
    dx::extension::tick(0.25f);
    REQUIRE(window->display.size() == 1);
    CHECK(window->display[0].ch == 'a');
    CHECK(window->display[0].time == Catch::Approx(0.75f));
    dx::extension::tick(1.0f);
    CHECK(window->display[0].time == 0.0f);
}

TEST_CASE("Keys reach no window when nothing has focus and no modal is open", "[data]")
{
    dx::UObject* root = makeRoot("DeusEx.DeusExRootWindow");
    CHECK(root->cls->name == dx::Name("DeusExRootWindow"));
    CHECK_FALSE(dx::extension::keyEvent(dx::IK_Escape, dx::IST_Press, 0));
    CHECK_FALSE(dx::extension::keyEvent(dx::IK_Escape, dx::IST_Release, 0));
}

TEST_CASE("SetChildVisibility changes only the window it is called on", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* parent = newChild(root, "Extension.Window");
    dx::UObject* first = newChild(parent, "Extension.Window");
    dx::UObject* second = newChild(parent, "Extension.Window");
    voidCall(first, "Hide");
    voidCall(second, "Hide");
    {
        dx::vm::Call call(parent, dx::Name("SetChildVisibility"));
        call.b(true);
        call.run();
    }
    CHECK(dx::window::bIsVisible(first) == 0);
    CHECK(dx::window::bIsVisible(second) == 0);
    {
        dx::vm::Call call(first, dx::Name("SetChildVisibility"));
        call.b(true);
        call.run();
    }
    CHECK(dx::window::bIsVisible(first) == 1);
    CHECK(dx::window::bIsVisible(second) == 0);
}

TEST_CASE("Setting a slider value updates the value text of a menu slider", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* slider = newChild(root, "DeusEx.MenuUISliderButtonWindow");
    REQUIRE(slider != nullptr);
    {
        dx::vm::Call call(slider, dx::Name("SetTicks"));
        call.i(11).i(0).i(10);
        call.run();
    }
    dx::UObject* scale = *reinterpret_cast<dx::UObject**>(slider->propertyAddress(dx::Name("winSlider")));
    dx::UObject* text = *reinterpret_cast<dx::UObject**>(slider->propertyAddress(dx::Name("winScaleText")));
    REQUIRE(scale != nullptr);
    REQUIRE(text != nullptr);
    {
        dx::vm::Call call(scale, dx::Name("SetValue"));
        call.f(7);
        call.run();
    }
    CHECK(*reinterpret_cast<std::string*>(text->propertyAddress(dx::Name("buttonText"))) == "7");
}

TEST_CASE("A menu slider from 0 to 1 shows the fractional value of its tick", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* slider = newChild(root, "DeusEx.MenuUISliderButtonWindow");
    REQUIRE(slider != nullptr);
    {
        dx::vm::Call call(slider, dx::Name("SetTicks"));
        call.i(21).i(0).i(1);
        call.run();
    }
    dx::UObject* scale = *reinterpret_cast<dx::UObject**>(slider->propertyAddress(dx::Name("winSlider")));
    dx::UObject* text = *reinterpret_cast<dx::UObject**>(slider->propertyAddress(dx::Name("winScaleText")));
    REQUIRE(scale != nullptr);
    REQUIRE(text != nullptr);
    {
        dx::vm::Call call(scale, dx::Name("SetValue"));
        call.f(0.55f);
        call.run();
    }
    CHECK(*reinterpret_cast<std::string*>(text->propertyAddress(dx::Name("buttonText"))) == "0.55");
}

TEST_CASE("Tab moves focus to the first window of the tab group and Down steps to the next", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    dx::UObject* first = newChild(modal, "Extension.ButtonWindow");
    dx::UObject* second = newChild(modal, "Extension.ButtonWindow");
    setSize(modal, 200, 200);
    setPos(first, 10, 10);
    setSize(first, 100, 20);
    setPos(second, 10, 50);
    setSize(second, 100, 20);
    layout(800, 600);
    {
        dx::vm::Call call(modal, dx::Name("SetFocusWindow"));
        call.obj(second);
        call.run();
    }
    REQUIRE(objectCall(root, "GetFocusWindow") == second);
    dx::extension::keyEvent(dx::IK_Tab, dx::IST_Press, 0);
    dx::extension::keyEvent(dx::IK_Tab, dx::IST_Release, 0);
    CHECK(objectCall(root, "GetFocusWindow") == first);
    dx::extension::keyEvent(dx::IK_Down, dx::IST_Press, 0);
    dx::extension::keyEvent(dx::IK_Down, dx::IST_Release, 0);
    CHECK(objectCall(root, "GetFocusWindow") == second);
}

TEST_CASE("GC text extent with zero width measures without wrapping", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* window = newChild(root, "Extension.Window");
    dx::UObject* gc = objectCall(window, "GetGC");
    REQUIRE(gc != nullptr);
    dx::UClass* canvasClass = dx::objects::findClass("Engine.Canvas");
    REQUIRE(canvasClass != nullptr);
    dx::UProperty* smallFont = canvasClass->findProperty(dx::Name("SmallFont"));
    REQUIRE(smallFont != nullptr);
    dx::UObject* font = *reinterpret_cast<dx::UObject**>(canvasClass->defaultObject->data + smallFont->offset);
    REQUIRE(font != nullptr);
    {
        dx::vm::Call setFont(gc, dx::Name("SetFont"));
        REQUIRE(setFont.valid());
        setFont.obj(font);
        setFont.run();
    }
    {
        dx::vm::Call wrap(gc, dx::Name("EnableWordWrap"));
        REQUIRE(wrap.valid());
        wrap.b(true);
        wrap.run();
    }
    dx::vm::Call extent(gc, dx::Name("GetTextExtent"));
    REQUIRE(extent.valid());
    extent.f(0).f(0).f(0).s("Hello World");
    extent.run();
    CHECK(*reinterpret_cast<float*>(extent.parameter(1)) == 66.0f);
    CHECK(*reinterpret_cast<float*>(extent.parameter(2)) == 9.0f);
    dx::vm::Call release(window, dx::Name("ReleaseGC"));
    release.obj(gc);
    release.run();
}

TEST_CASE("modulated GC style leaves text drawing unmodulated", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* window = newChild(root, "Extension.Window");
    dx::UObject* gc = objectCall(window, "GetGC");
    REQUIRE(gc != nullptr);
    dx::vm::Call style(gc, dx::Name("SetStyle"));
    REQUIRE(style.valid());
    style.byte(4);
    style.run();
    CHECK(dx::gc::bModulated(gc) == 1);
    CHECK(dx::gc::textPolyFlags(gc) == 0);
    CHECK(dx::gc::bTextTranslucent(gc) == 0);
    dx::vm::Call release(window, dx::Name("ReleaseGC"));
    release.obj(gc);
    release.run();
}

TEST_CASE("Conversation sound length uses the first MP3 frame bitrate over the whole data size", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UObject* sound = dx::objects::loadObject("DeusExConAudioMission01.ConAudioMission01_576", nullptr);
    dx::UClass* eventClass = dx::objects::findClass("ConSys.ConEventSpeech");
    dx::objects::flushLoads();
    REQUIRE(sound != nullptr);
    REQUIRE(eventClass != nullptr);
    dx::UObject* event = dx::objects::newObject(eventClass, dx::objects::packageObject(dx::Name("Transient")), dx::Name("SoundLengthEvent"), 0);
    dx::vm::Call call(event, dx::Name("GetSoundLength"));
    call.obj(sound);
    call.run();
    CHECK(*reinterpret_cast<float*>(call.returnValue()) == Catch::Approx(61464.0f * 8.0f / 48000.0f));
}

TEST_CASE("Accelerator keys activate a button only while Alt is held", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    dx::UObject* box = newChild(modal, "Extension.CheckboxWindow");
    setText(box, "|&Box");
    setSize(modal, 200, 200);
    setSize(box, 100, 20);
    layout(800, 600);
    dx::extension::charEvent('b');
    {
        dx::vm::Call call(box, dx::Name("GetToggle"));
        CHECK_FALSE(boolResult(call));
    }
    dx::extension::keyEvent(dx::IK_Alt, dx::IST_Press, 0);
    dx::extension::charEvent('b');
    dx::extension::keyEvent(dx::IK_Alt, dx::IST_Release, 0);
    {
        dx::vm::Call call(box, dx::Name("GetToggle"));
        CHECK(boolResult(call));
    }
}

TEST_CASE("Root window is the current modal while no modal window is open", "[data]")
{
    dx::UObject* root = makeRoot();
    {
        dx::vm::Call call(root, dx::Name("IsCurrentModal"));
        CHECK(boolResult(call));
    }
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    {
        dx::vm::Call call(root, dx::Name("IsCurrentModal"));
        CHECK_FALSE(boolResult(call));
    }
    {
        dx::vm::Call call(modal, dx::Name("IsCurrentModal"));
        CHECK(boolResult(call));
    }
}

TEST_CASE("Edit window drops characters rejected by the script FilterChar", "[data]")
{
    dx::UObject* root = makeRoot("DeusEx.DeusExRootWindow");
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    dx::UObject* edit = newChild(modal, "DeusEx.MenuUIEditWindow");
    REQUIRE(edit != nullptr);
    {
        dx::vm::Call call(edit, dx::Name("SetFilter"));
        call.s("ab");
        call.run();
    }
    {
        dx::vm::Call call(edit, dx::Name("InsertText"));
        call.s("abcab").b(true).b(false);
        CHECK(boolResult(call));
    }
    CHECK(dx::textwindow::Text(edit) == "abab");
    {
        dx::vm::Call call(edit, dx::Name("InsertText"));
        call.s("c").b(true).b(false);
        CHECK_FALSE(boolResult(call));
    }
    {
        dx::vm::Call call(root, dx::Name("SetFocusWindow"));
        call.obj(edit);
        call.run();
    }
    REQUIRE(objectCall(root, "GetFocusWindow") == edit);
    dx::extension::charEvent('x');
    dx::extension::charEvent('b');
    CHECK(dx::textwindow::Text(edit) == "ababb");
}

TEST_CASE("a list row is activated when the second click of a double-click is released", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    {
        dx::vm::Call call(modal, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    dx::UObject* list = newChild(modal, "Extension.ListWindow");
    setPos(list, 100, 100);
    setSize(list, 200, 100);
    for (const char* text : { "first", "second" }) {
        dx::vm::Call call(list, dx::Name("AddRow"));
        call.s(text).i(0);
        call.run();
    }
    layout(800, 600);
    // Activation plays the list's activate sound, so count those.
    int activations = 0;
    dx::listwindow::ActivateSound(list) = list;
    dx::extension::setSoundPlayer([&](dx::UObject* sound, dx::UObject*, float, float) {
        if (sound == list) {
            activations++;
        }
    });
    dx::extension::mouseMove(110, 102);
    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    CHECK(activations == 0);
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    CHECK(activations == 1);
    dx::extension::setSoundPlayer(nullptr);
}

TEST_CASE("MoveRow with drag extends the selection from the anchor", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* list = newChild(root, "Extension.ListWindow");
    {
        dx::vm::Call call(list, dx::Name("EnableMultiSelect"));
        call.b(true);
        call.run();
    }
    for (const char* text : { "a", "b", "c", "d" }) {
        dx::vm::Call call(list, dx::Name("AddRow"));
        call.s(text).i(0);
        call.run();
    }
    auto rowId = [&](int32_t index) {
        dx::vm::Call call(list, dx::Name("IndexToRowId"));
        return call.i(index).result<int32_t>();
    };
    auto selected = [&](int32_t index) {
        dx::vm::Call call(list, dx::Name("IsRowSelected"));
        return call.i(rowId(index)).resultBool();
    };
    {
        dx::vm::Call call(list, dx::Name("SetRow"));
        call.i(rowId(0)).b(true).b(true);
        call.run();
    }
    for (int i = 0; i < 2; i++) {
        dx::vm::Call call(list, dx::Name("MoveRow"));
        call.byte(1).b(true).b(true).b(true);
        call.run();
    }
    CHECK(selected(0));
    CHECK(selected(1));
    CHECK(selected(2));
    CHECK_FALSE(selected(3));
    // Moving back up shrinks the range toward the anchor.
    {
        dx::vm::Call call(list, dx::Name("MoveRow"));
        call.byte(0).b(true).b(true).b(true);
        call.run();
    }
    CHECK(selected(1));
    CHECK_FALSE(selected(2));
}

TEST_CASE("Showing a parent selects the first row of a list that became visible with it", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* parent = static_cast<dx::extension::UWindow*>(root)->newChild(dx::objects::findClass("Extension.Window"), false);
    CHECK(dx::window::bIsVisible(parent) == 0);
    REQUIRE(parent != nullptr);
    dx::UObject* list = newChild(parent, "Extension.ListWindow");
    int32_t firstRow = 0;
    {
        dx::vm::Call call(list, dx::Name("AddRow"));
        call.s("first").i(0);
        call.run();
        firstRow = *reinterpret_cast<int32_t*>(call.returnValue());
    }
    {
        dx::vm::Call call(list, dx::Name("AddRow"));
        call.s("second").i(0);
        call.run();
    }
    {
        dx::vm::Call call(list, dx::Name("GetSelectedRow"));
        call.run();
        CHECK(*reinterpret_cast<int32_t*>(call.returnValue()) == 0);
    }
    static_cast<dx::extension::UWindow*>(parent)->show(true);
    {
        dx::vm::Call call(list, dx::Name("GetSelectedRow"));
        call.run();
        CHECK(*reinterpret_cast<int32_t*>(call.returnValue()) == firstRow);
    }
}

TEST_CASE("VisibilityChanged reaches a window created visible and its descendants when an ancestor is shown", "[data]")
{
    dx::UObject* root = makeRoot("DeusEx.DeusExRootWindow");
    dx::UObject* compass = newChild(root, "DeusEx.HUDCompassDisplay");
    CHECK(dx::window::bTickEnabled(compass) == 1);
    dx::UObject* parent = static_cast<dx::extension::UWindow*>(root)->newChild(dx::objects::findClass("Extension.Window"), false);
    CHECK(dx::window::bIsVisible(parent) == 0);
    dx::UObject* inner = newChild(parent, "DeusEx.HUDCompassDisplay");
    CHECK(dx::window::bTickEnabled(inner) == 0);
    static_cast<dx::extension::UWindow*>(parent)->show(true);
    CHECK(dx::window::bTickEnabled(inner) == 1);
    static_cast<dx::extension::UWindow*>(parent)->show(false);
    CHECK(dx::window::bTickEnabled(inner) == 0);
}

TEST_CASE("ConvertVectorToCoordinates succeeds for points in front of the view even when they fall outside the window", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::extension::setViewportSize(800, 600);
    layout(800, 600);
    dx::UObject* frame = newChild(root, "Extension.Window");
    setPos(frame, 10, 20);
    setSize(frame, 100, 100);
    layout(800, 600);
    dx::extension::setProjector([](const dx::Vector& location, float& pixelX, float& pixelY) {
        pixelX = location.x;
        pixelY = location.y;
        return location.z > 0;
    });
    float outX = 0;
    float outY = 0;
    {
        dx::vm::Call call(frame, dx::Name("ConvertVectorToCoordinates"));
        call.v({ -500.75f, 3000.5f, 1 });
        *reinterpret_cast<float*>(call.parameter(1)) = 0;
        *reinterpret_cast<float*>(call.parameter(2)) = 0;
        CHECK(boolResult(call));
        outX = *reinterpret_cast<float*>(call.parameter(1));
        outY = *reinterpret_cast<float*>(call.parameter(2));
    }
    CHECK(outX == -510.0f);
    CHECK(outY == 2980.0f);
    {
        dx::vm::Call call(frame, dx::Name("ConvertVectorToCoordinates"));
        call.v({ 50, 50, -1 });
        CHECK_FALSE(boolResult(call));
        CHECK(*reinterpret_cast<float*>(call.parameter(1)) == 390.0f);
        CHECK(*reinterpret_cast<float*>(call.parameter(2)) == 280.0f);
    }
    dx::extension::setProjector(nullptr);
}

TEST_CASE("Checkbox text uses the colors set with SetTextColors", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* box = newChild(root, "Extension.CheckboxWindow");
    setText(box, "He");
    setPos(box, 100, 50);
    setSize(box, 80, 20);
    {
        dx::vm::Call call(box, dx::Name("SetTextColors"));
        dx::Color red { 255, 0, 0, 255 };
        for (int i = 0; i < 6; i++) {
            *reinterpret_cast<dx::Color*>(call.parameter(static_cast<size_t>(i))) = red;
        }
        call.run();
    }
    RecordingCanvas canvas(800, 600);
    dx::extension::render(canvas);
    int red = 0;
    int green = 0;
    for (const RecordedTile& tile : canvas.tiles) {
        if (tile.color.r == 1.0f && tile.color.g == 0.0f) {
            red++;
        }
        if (tile.color.r == 0.0f && tile.color.g == 1.0f) {
            green++;
        }
    }
    CHECK(red == 2);
    CHECK(green == 0);
}

TEST_CASE("Edit window Up and Down keep the caret column across rows", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* edit = newChild(root, "Extension.EditWindow");
    setSize(edit, 300, 100);
    layout(800, 600);
    setText(edit, "abcdef\nab");
    auto move = [&](uint8_t where) {
        dx::vm::Call call(edit, dx::Name("MoveInsertionPoint"));
        call.byte(where).b(false);
        call.run();
    };
    auto position = [&]() {
        dx::vm::Call call(edit, dx::Name("GetInsertionPoint"));
        call.run();
        return *reinterpret_cast<int32_t*>(call.returnValue());
    };
    move(11);
    REQUIRE(position() == 9);
    move(0);
    CHECK(position() == 2);
    move(3);
    move(3);
    CHECK(position() == 4);
    move(1);
    CHECK(position() == 9);
    move(0);
    CHECK(position() == 4);
    move(1);
    move(6);
    CHECK(position() == 7);
}

TEST_CASE("Edit window SetText clears undo, marks the text changed and puts the caret at the start", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* edit = newChild(root, "Extension.EditWindow");
    setText(edit, "hello");
    {
        dx::vm::Call call(edit, dx::Name("GetInsertionPoint"));
        call.run();
        CHECK(*reinterpret_cast<int32_t*>(call.returnValue()) == 0);
    }
    {
        dx::vm::Call call(edit, dx::Name("HasTextChanged"));
        CHECK(boolResult(call));
    }
    voidCall(edit, "Undo");
    CHECK(dx::textwindow::Text(edit) == "hello");
}

TEST_CASE("Undoing back to the unchanged text clears the edit window changed flag", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* edit = newChild(root, "Extension.EditWindow");
    setText(edit, "ab");
    voidCall(edit, "ClearTextChangedFlag");
    {
        dx::vm::Call call(edit, dx::Name("SetInsertionPoint"));
        call.i(2).b(false);
        call.run();
    }
    {
        dx::vm::Call call(edit, dx::Name("InsertText"));
        call.s("c").b(true).b(false);
        CHECK(boolResult(call));
    }
    CHECK(dx::textwindow::Text(edit) == "abc");
    {
        dx::vm::Call call(edit, dx::Name("HasTextChanged"));
        CHECK(boolResult(call));
    }
    voidCall(edit, "Undo");
    CHECK(dx::textwindow::Text(edit) == "ab");
    {
        dx::vm::Call call(edit, dx::Name("HasTextChanged"));
        CHECK_FALSE(boolResult(call));
    }
}

namespace {
    std::pair<int32_t, int32_t> selectedArea(dx::UObject* edit)
    {
        dx::vm::Call call(edit, dx::Name("GetSelectedArea"));
        call.i(-5).i(-5);
        call.run();
        return { *reinterpret_cast<int32_t*>(call.parameter(0)), *reinterpret_cast<int32_t*>(call.parameter(1)) };
    }
}

TEST_CASE("Edit window typing is one undo step, and undo and redo select what they put back", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* edit = newChild(root, "Extension.EditWindow");
    setText(edit, "x ");
    voidCall(edit, "ClearTextChangedFlag");
    {
        dx::vm::Call call(edit, dx::Name("SetInsertionPoint"));
        call.i(2).b(false);
        call.run();
    }
    for (const char* c : { "a", "b", "c" }) {
        dx::vm::Call call(edit, dx::Name("InsertText"));
        call.s(c).b(true).b(false);
        call.run();
    }
    REQUIRE(dx::textwindow::Text(edit) == "x abc");
    CHECK(selectedArea(edit) == std::pair<int32_t, int32_t>(0, 0));
    voidCall(edit, "Undo");
    CHECK(dx::textwindow::Text(edit) == "x ");
    voidCall(edit, "Redo");
    CHECK(dx::textwindow::Text(edit) == "x abc");
    CHECK(selectedArea(edit) == std::pair<int32_t, int32_t>(2, 3));
    // Typing over the selection is a new step; undoing it puts "abc" back selected.
    {
        dx::vm::Call call(edit, dx::Name("InsertText"));
        call.s("d").b(true).b(false);
        call.run();
    }
    CHECK(dx::textwindow::Text(edit) == "x d");
    voidCall(edit, "Undo");
    CHECK(dx::textwindow::Text(edit) == "x abc");
    CHECK(selectedArea(edit) == std::pair<int32_t, int32_t>(2, 3));
}

TEST_CASE("Edit window double-click selects a word and dragging extends the selection", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    {
        dx::vm::Call call(modal, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    dx::UObject* edit = newChild(modal, "Extension.EditWindow");
    setPos(edit, 100, 100);
    setSize(edit, 300, 20);
    layout(800, 600);
    setText(edit, "hello world");
    dx::extension::mouseMove(390, 105);
    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    CHECK(selectedArea(edit) == std::pair<int32_t, int32_t>(0, 0));
    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    CHECK(selectedArea(edit) == std::pair<int32_t, int32_t>(6, 5));
    // A later single press starts a plain drag from the end to the left edge.
    dx::extension::mouseMove(395, 300);
    dx::extension::mouseMove(390, 105);
    dx::extension::tick(1.0f);
    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    dx::extension::mouseMove(60, 105);
    CHECK(selectedArea(edit) == std::pair<int32_t, int32_t>(0, 11));
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    CHECK(selectedArea(edit) == std::pair<int32_t, int32_t>(0, 11));
}

TEST_CASE("A press grabs the mouse for the clicked window and every release lets go", "[data]")
{
    // XRootWindow::HandleButtons. MenuUIWindow grabs the mouse to drag itself and never
    // lets go; the release has to.
    dx::UObject* root = makeRoot();
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    {
        dx::vm::Call call(modal, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    dx::UObject* first = newChild(modal, "Extension.Window");
    setPos(first, 100, 100);
    setSize(first, 50, 50);
    dx::UObject* second = newChild(modal, "Extension.Window");
    setPos(second, 300, 100);
    setSize(second, 50, 50);
    layout(800, 600);
    dx::extension::mouseMove(120, 120);
    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    CHECK(dx::rootwindow::grabbedWindow(root) == first);
    dx::extension::mouseMove(320, 120);
    CHECK(dx::rootwindow::grabbedWindow(root) == first);
    dx::vm::Call(second, dx::Name("GrabMouse")).run();
    CHECK(dx::rootwindow::grabbedWindow(root) == second);
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    CHECK(dx::rootwindow::grabbedWindow(root) == nullptr);
}

TEST_CASE("Clicks go only to the topmost modal, and insensitive windows pass them to their parent", "[data]")
{
    // XRootWindow::GetMouseWindow and XWindow::FindWindowByPoint.
    dx::UObject* root = makeRoot();
    dx::UObject* behind = newChild(root, "Extension.ButtonWindow");
    setPos(behind, 20, 20);
    setSize(behind, 50, 50);
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    setPos(modal, 200, 200);
    setSize(modal, 200, 200);
    dx::UObject* inside = newChild(modal, "Extension.ButtonWindow");
    setPos(inside, 10, 10);
    setSize(inside, 50, 50);
    layout(800, 600);
    auto pressedOn = [](float x, float y, dx::UObject* button) {
        dx::extension::mouseMove(x, y);
        dx::extension::mouseButton(dx::IK_LeftMouse, true);
        bool pressed = dx::buttonwindow::bButtonPressed(button) != 0;
        dx::extension::mouseButton(dx::IK_LeftMouse, false);
        return pressed;
    };
    CHECK_FALSE(pressedOn(40, 40, behind));
    CHECK(pressedOn(230, 230, inside));
    dx::vm::Call(inside, dx::Name("SetSensitivity")).b(false).run();
    CHECK_FALSE(pressedOn(230, 230, inside));
}

TEST_CASE("Opening the first modal window forgets keys held before it, but not mouse buttons", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::extension::keyEvent(dx::IK_Shift, dx::IST_Press, 0);
    dx::extension::keyEvent(dx::IK_LeftMouse, dx::IST_Press, 0);
    auto down = [&](uint8_t key) {
        dx::vm::Call call(root, dx::Name("IsKeyDown"));
        call.byte(key);
        return boolResult(call);
    };
    REQUIRE(down(dx::IK_Shift));
    newChild(root, "Extension.ModalWindow");
    CHECK_FALSE(down(dx::IK_Shift));
    CHECK(down(dx::IK_LeftMouse));
    dx::extension::keyEvent(dx::IK_Shift, dx::IST_Press, 0);
    newChild(root, "Extension.ModalWindow");
    CHECK(down(dx::IK_Shift));
    dx::extension::keyEvent(dx::IK_Shift, dx::IST_Release, 0);
    dx::extension::keyEvent(dx::IK_LeftMouse, dx::IST_Release, 0);
}

TEST_CASE("Edit window caret stays on for the blink start delay then toggles every half period", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* edit = newChild(root, "Extension.EditWindow");
    setSize(edit, 200, 20);
    {
        dx::vm::Call call(root, dx::Name("SetFocusWindow"));
        call.obj(edit);
        call.run();
    }
    REQUIRE(objectCall(root, "GetFocusWindow") == edit);
    dx::extension::tick(5.0f);
    {
        dx::vm::Call call(edit, dx::Name("InsertText"));
        call.s("a").b(true).b(false);
        call.run();
    }
    auto showing = [&](float advance) {
        dx::extension::tick(advance);
        layout(800, 600);
        return dx::editwindow::bCursorShowing(edit) != 0;
    };
    CHECK(showing(0.0f));
    CHECK(showing(0.7f));
    CHECK_FALSE(showing(0.1f));
    CHECK(showing(0.5f));
    CHECK_FALSE(showing(0.5f));
}

TEST_CASE("A new modal window focuses its top tab group even before it has been drawn", "[data]")
{
    dx::UObject* root = makeRoot();
    layout(800, 600);
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    setSize(modal, 400, 400);
    dx::UObject* button = newChild(modal, "Extension.ButtonWindow");
    setPos(button, 10, 350);
    setSize(button, 100, 20);
    dx::UObject* clip = newChild(modal, "Extension.ClipWindow");
    setPos(clip, 10, 10);
    setSize(clip, 300, 200);
    dx::UObject* list = newChild(clip, "Extension.ListWindow");
    {
        dx::vm::Call call(list, dx::Name("AddRow"));
        call.s("row").i(0);
        call.run();
    }
    dx::extension::tick(0.01f);
    CHECK(objectCall(root, "GetFocusWindow") == list);
}

TEST_CASE("Keys only reach windows while a modal window has grabbed the keyboard", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* box = newChild(root, "Extension.CheckboxWindow");
    setSize(box, 40, 20);
    layout(800, 600);
    {
        dx::vm::Call call(root, dx::Name("SetFocusWindow"));
        call.obj(box);
        call.run();
    }
    REQUIRE(objectCall(root, "GetFocusWindow") == box);
    CHECK_FALSE(dx::extension::keyEvent(dx::IK_Enter, dx::IST_Press, 0));
    dx::extension::keyEvent(dx::IK_Enter, dx::IST_Release, 0);
    {
        dx::vm::Call call(box, dx::Name("GetToggle"));
        CHECK_FALSE(boolResult(call));
    }
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    setSize(modal, 200, 200);
    dx::UObject* inner = newChild(modal, "Extension.CheckboxWindow");
    setSize(inner, 40, 20);
    layout(800, 600);
    {
        dx::vm::Call call(root, dx::Name("SetFocusWindow"));
        call.obj(inner);
        call.run();
    }
    REQUIRE(objectCall(root, "GetFocusWindow") == inner);
    CHECK(dx::extension::keyEvent(dx::IK_Enter, dx::IST_Press, 0));
    dx::extension::keyEvent(dx::IK_Enter, dx::IST_Release, 0);
    {
        dx::vm::Call call(inner, dx::Name("GetToggle"));
        CHECK(boolResult(call));
    }
}

TEST_CASE("The cursor shows the icon of an inventory button while it is being dragged", "[data]")
{
    dx::UObject* root = makeRoot("DeusEx.DeusExRootWindow");
    voidCall(root, "DestroyAllChildren");
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    {
        dx::vm::Call call(modal, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    dx::UClass* medkit = dx::objects::findClass("DeusEx.MedKit");
    REQUIRE(medkit != nullptr);
    dx::objects::ensureLoaded(medkit);
    REQUIRE(medkit->defaultObject != nullptr);
    dx::UObject* icon = *reinterpret_cast<dx::UObject**>(medkit->defaultObject->propertyAddress(dx::Name("largeIcon")));
    REQUIRE(icon != nullptr);
    dx::UObject* button = newChild(modal, "DeusEx.PersonaInventoryItemButton");
    setPos(button, 80, 180);
    {
        dx::vm::Call call(button, dx::Name("SetIcon"));
        call.obj(icon);
        call.run();
    }
    {
        dx::vm::Call call(button, dx::Name("SetIconSize"));
        call.i(39).i(46);
        call.run();
    }
    layout(800, 600);
    dx::extension::mouseMove(100, 200);
    voidCall(button, "StartButtonDrag");
    RecordingCanvas canvas(800, 600);
    dx::extension::render(canvas);
    const RecordedTile* dragged = nullptr;
    for (const RecordedTile& tile : canvas.tiles) {
        if (tile.texture == icon) {
            dragged = &tile;
        }
    }
    REQUIRE(dragged != nullptr);
    CHECK(dragged->x == 81.0f);
    CHECK(dragged->y == 177.0f);
}

TEST_CASE("A cursor set without a hot spot is centered on the mouse position", "[data]")
{
    dx::UObject* root = makeRoot("DeusEx.DeusExRootWindow");
    voidCall(root, "DestroyAllChildren");
    newChild(root, "Extension.ModalWindow");
    layout(800, 600);
    dx::extension::mouseMove(100, 200);
    RecordingCanvas canvas(800, 600);
    dx::extension::render(canvas);
    const RecordedTile* cursor = nullptr;
    for (const RecordedTile& tile : canvas.tiles) {
        if (tile.texture && tile.texture->name == dx::Name("DeusExCursor1")) {
            cursor = &tile;
        }
    }
    REQUIRE(cursor != nullptr);
    CHECK(cursor->x == 69.0f);
    CHECK(cursor->y == 169.0f);
}

namespace {
    int32_t buttonY(dx::UObject* menu, int index)
    {
        dx::UStruct* button = menu->cls->findProperty(dx::Name("buttonDefaults"))->structType;
        return *reinterpret_cast<int32_t*>(menu->propertyAddress(dx::Name("buttonDefaults"), index) + button->findProperty(dx::Name("Y"))->offset);
    }

    dx::UObject* buttonInvoke(dx::UObject* menu, int index)
    {
        dx::UStruct* button = menu->cls->findProperty(dx::Name("buttonDefaults"))->structType;
        return *reinterpret_cast<dx::UObject**>(menu->propertyAddress(dx::Name("buttonDefaults"), index) + button->findProperty(dx::Name("Invoke"))->offset);
    }

    dx::UTexture* clientTexture(dx::UObject* menu, int index)
    {
        dx::UObject* texture = *reinterpret_cast<dx::UObject**>(menu->propertyAddress(dx::Name("clientTextures"), index));
        dx::objects::ensureLoaded(texture);
        return dx::cast<dx::UTexture>(texture);
    }

    void checkRow(dx::UTexture* result, int resultY, dx::UTexture* source, int sourceY)
    {
        std::vector<uint8_t> resultPixels = result->rgba(0);
        std::vector<uint8_t> sourcePixels = source->rgba(0);
        REQUIRE(result->width() == source->width());
        int mismatches = 0;
        for (uint32_t x = 0; x < source->width(); x++) {
            size_t from = (static_cast<size_t>(sourceY) * source->width() + x);
            size_t to = (static_cast<size_t>(resultY) * result->width() + x) * 4;
            bool transparent = source->mips[0].data[from] == 0;
            if (transparent ? resultPixels[to + 3] != 0 : (resultPixels[to + 3] != 255 || resultPixels[to] != sourcePixels[from * 4] || resultPixels[to + 1] != sourcePixels[from * 4 + 1] || resultPixels[to + 2] != sourcePixels[from * 4 + 2])) {
                mismatches++;
            }
        }
        CHECK(mismatches == 0);
    }
}

TEST_CASE("Inserting a menu button shifts the later buttons down one slot and repeats a slot of the background", "[data]")
{
    initializeEngine();
    dx::UClass* settings = dx::objects::findClass("DeusEx.MenuSettings");
    dx::UClass* target = dx::objects::findClass("DeusEx.MenuScreenOptions");
    dx::UObject* menu = dx::objects::createSubclass(settings, dx::Name("TestSettingsMenu"))->defaultObject;
    std::vector<dx::UTexture*> source;
    for (int i = 0; i < 4; i++) {
        source.push_back(clientTexture(menu, i));
    }
    dx::deusex::insertMenuButton(menu, 3, "Extra", target);

    CHECK(*reinterpret_cast<std::string*>(menu->propertyAddress(dx::Name("ButtonNames"), 2)) == "Game Options");
    CHECK(*reinterpret_cast<std::string*>(menu->propertyAddress(dx::Name("ButtonNames"), 3)) == "Extra");
    CHECK(*reinterpret_cast<std::string*>(menu->propertyAddress(dx::Name("ButtonNames"), 4)) == "Display");
    CHECK(*reinterpret_cast<std::string*>(menu->propertyAddress(dx::Name("ButtonNames"), 7)) == "Previous Menu");
    CHECK(buttonY(menu, 2) == 85);
    CHECK(buttonY(menu, 3) == 121);
    CHECK(buttonY(menu, 4) == 157);
    CHECK(buttonY(menu, 7) == 302);
    CHECK(buttonInvoke(menu, 3) == target);
    CHECK(buttonInvoke(menu, 4) == dx::objects::findClass("DeusEx.MenuScreenDisplay"));
    CHECK(*reinterpret_cast<int32_t*>(menu->propertyAddress(dx::Name("ClientHeight"))) == 344);
    CHECK(*reinterpret_cast<int32_t*>(menu->propertyAddress(dx::Name("textureRows"))) == 2);

    dx::UTexture* top = clientTexture(menu, 0);
    dx::UTexture* bottom = clientTexture(menu, 2);
    REQUIRE(top != source[0]);
    CHECK(top->height() == 256);
    // The bottom 12 rows of the art are empty and left off.
    CHECK(bottom->height() == 88);
    CHECK(clientTexture(menu, 1)->width() == 64);
    checkRow(top, 50, source[0], 50);
    checkRow(top, 130, source[0], 94);
    checkRow(top, 200, source[0], 164);
    checkRow(bottom, 44, source[2], 8);
    CHECK(clientTexture(settings->defaultObject, 0) == source[0]);
}

TEST_CASE("Inserting Gamepad directly below Controls preserves the settings row spacing", "[data]")
{
    initializeEngine();
    auto* settings = dx::objects::findClass("DeusEx.MenuSettings");
    auto* menu = dx::objects::createSubclass(settings, dx::Name("TestSecondRowMenu"))->defaultObject;
    int first = buttonY(menu, 0);
    int second = buttonY(menu, 1);
    REQUIRE(second > first);
    dx::deusex::insertMenuButton(menu, 1, "Gamepad", dx::objects::findClass("DeusEx.MenuScreenOptions"));
    CHECK(buttonY(menu, 0) == first);
    CHECK(buttonY(menu, 1) == second);
    CHECK(buttonY(menu, 2) == second + (second - first));
}

TEST_CASE("Inserting a second menu button keeps the art the first one spliced", "[data]")
{
    initializeEngine();
    dx::UClass* settings = dx::objects::findClass("DeusEx.MenuSettings");
    dx::UClass* target = dx::objects::findClass("DeusEx.MenuScreenOptions");
    dx::UObject* menu = dx::objects::createSubclass(settings, dx::Name("TestSettingsMenuTwice"))->defaultObject;
    dx::UTexture* source = clientTexture(menu, 0);
    dx::deusex::insertMenuButton(menu, 3, "Extra", target);
    dx::UTexture* once = clientTexture(menu, 0);
    dx::deusex::insertMenuButton(menu, 4, "Second", target);

    CHECK(*reinterpret_cast<int32_t*>(menu->propertyAddress(dx::Name("ClientHeight"))) == 380);
    dx::UTexture* top = clientTexture(menu, 0);
    REQUIRE(top != once);
    // Above the second slot the art is as the first splice left it, transparent pixels included.
    checkRow(top, 50, source, 50);
    checkRow(top, 140, source, 104);
    // The first splice's RGBA art is masked by its alpha, not read as palette indices.
    std::vector<uint8_t> before = once->rgba(0);
    std::vector<uint8_t> after = top->rgba(0);
    int mismatches = 0;
    for (size_t i = 0; i < static_cast<size_t>(140) * once->width() * 4; i++) {
        mismatches += before[i] != after[i];
    }
    CHECK(mismatches == 0);
}

TEST_CASE("Growing a menu's background repeats a band of its art and leaves off empty rows", "[data]")
{
    initializeEngine();
    dx::UClass* options = dx::objects::findClass("DeusEx.MenuScreenOptions");
    dx::UObject* menu = dx::objects::createSubclass(options, dx::Name("TestOptionsMenu"))->defaultObject;
    std::vector<dx::UTexture*> source;
    for (int i = 0; i < 6; i++) {
        source.push_back(clientTexture(menu, i));
    }
    dx::deusex::growMenuBackground(menu, 290, 36);

    CHECK(*reinterpret_cast<int32_t*>(menu->propertyAddress(dx::Name("ClientHeight"))) == 442);
    // The art is 512 rows of tiles, but nothing is drawn below the help bar, so with the band
    // it still fits in two rows of tiles.
    CHECK(*reinterpret_cast<int32_t*>(menu->propertyAddress(dx::Name("textureRows"))) == 2);
    dx::UTexture* top = clientTexture(menu, 0);
    dx::UTexture* bottom = clientTexture(menu, 3);
    CHECK(clientTexture(menu, 2)->width() == 32);
    checkRow(top, 100, source[0], 100);
    checkRow(top, 255, source[0], 255);
    // Row 290 starts the repeat of rows 254 to 289.
    checkRow(bottom, 33, source[3], 33);
    checkRow(bottom, 34, source[0], 254);
    checkRow(bottom, 70, source[3], 34);
    CHECK(clientTexture(options->defaultObject, 0) == source[0]);
}

TEST_CASE("The modal mouse focus mode decides whether entering and leaving windows moves the focus", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    {
        dx::vm::Call call(modal, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    dx::UObject* first = newChild(modal, "Extension.ButtonWindow");
    setPos(first, 100, 100);
    setSize(first, 80, 20);
    dx::UObject* second = newChild(modal, "Extension.ButtonWindow");
    setPos(second, 100, 200);
    setSize(second, 80, 20);
    layout(800, 600);
    auto setMode = [modal](uint8_t mode) {
        dx::vm::Call call(modal, dx::Name("SetMouseFocusMode"));
        call.byte(mode);
        call.run();
    };

    dx::extension::mouseMove(110, 210);
    CHECK(objectCall(root, "GetFocusWindow") == second);
    dx::extension::mouseMove(110, 110);
    CHECK(objectCall(root, "GetFocusWindow") == first);
    dx::extension::mouseMove(400, 400);
    CHECK(objectCall(root, "GetFocusWindow") == first);

    setMode(3);
    dx::extension::mouseMove(110, 210);
    CHECK(objectCall(root, "GetFocusWindow") == second);
    dx::extension::mouseMove(400, 400);
    CHECK(objectCall(root, "GetFocusWindow") == nullptr);

    setMode(1);
    dx::extension::mouseMove(110, 110);
    CHECK(objectCall(root, "GetFocusWindow") == nullptr);
}

TEST_CASE("A click moves the focus to the nearest selectable window unless the modal's focus mode is none", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    {
        dx::vm::Call call(modal, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    dx::UObject* button = newChild(modal, "Extension.ButtonWindow");
    setPos(button, 100, 100);
    setSize(button, 80, 40);
    dx::UObject* inner = newChild(button, "Extension.Window");
    setPos(inner, 10, 10);
    setSize(inner, 20, 20);
    layout(800, 600);
    auto setMode = [modal](uint8_t mode) {
        dx::vm::Call call(modal, dx::Name("SetMouseFocusMode"));
        call.byte(mode);
        call.run();
    };

    // MFOCUS_Click: clicking the window inside the button focuses the button.
    setMode(1);
    dx::extension::mouseMove(115, 115);
    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    CHECK(objectCall(root, "GetFocusWindow") == button);

    // MFOCUS_None: clicks leave the focus alone.
    {
        dx::vm::Call call(root, dx::Name("SetFocusWindow"));
        call.obj(nullptr);
        call.run();
    }
    setMode(0);
    dx::extension::mouseButton(dx::IK_LeftMouse, true);
    dx::extension::mouseButton(dx::IK_LeftMouse, false);
    CHECK(objectCall(root, "GetFocusWindow") == nullptr);
}

TEST_CASE("A second click only counts as a double-click near the first", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    {
        dx::vm::Call call(modal, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    layout(800, 600);
    CHECK(dx::rootwindow::multiClickTimeout(root) == 0.5f);
    CHECK(dx::rootwindow::maxMouseDist(root) == 10.0f);
    auto click = [] {
        dx::extension::mouseButton(dx::IK_LeftMouse, true);
        dx::extension::mouseButton(dx::IK_LeftMouse, false);
    };

    dx::extension::mouseMove(200, 200);
    click();
    CHECK(dx::rootwindow::clickCount(root) == 0);
    // Within 10 units on each axis of the first press: a double-click.
    dx::extension::mouseMove(208, 195);
    click();
    CHECK(dx::rootwindow::clickCount(root) == 1);
    // Further than that from the first press: a new single click.
    dx::extension::mouseMove(230, 200);
    click();
    CHECK(dx::rootwindow::clickCount(root) == 0);
    click();
    CHECK(dx::rootwindow::clickCount(root) == 1);
}

TEST_CASE("The root window keeps at least 600 virtual rows at 1080p and 4K", "[data]")
{
    auto* root = static_cast<dx::extension::UWindow*>(makeRoot());
    dx::extension::setViewportSize(1920, 1080);
    layout(1920, 1080);
    CHECK(root->width() == 1440.0f);
    CHECK(root->height() == 1080.0f);

    dx::extension::setViewportSize(3840, 2160);
    layout(3840, 2160);
    CHECK(root->width() == 960.0f);
    CHECK(root->height() == 720.0f);
    dx::extension::setViewportSize(800, 600);
}

TEST_CASE("A forced UI scale overrides the automatic scale even below 600 virtual rows", "[data]")
{
    auto* root = static_cast<dx::extension::UWindow*>(makeRoot());
    dx::extension::setUIScale(2);
    dx::extension::setViewportSize(1920, 1080);
    layout(1920, 1080);
    CHECK(root->width() == 720.0f);
    CHECK(root->height() == 540.0f);

    dx::extension::setUIScale(1);
    dx::extension::setViewportSize(3840, 2160);
    layout(3840, 2160);
    CHECK(root->width() == 2880.0f);
    CHECK(root->height() == 2160.0f);
    dx::extension::setUIScale(0);
    dx::extension::setViewportSize(800, 600);
}

TEST_CASE("Conversation letterbox bars cover an ultrawide screen while the conversation stays in the 16:9 area", "[data]")
{
    dx::UObject* root = makeRoot("DeusEx.DeusExRootWindow");
    voidCall(root, "DestroyAllChildren");
    dx::extension::setViewportSize(1720, 720);
    layout(1720, 720);
    auto* conversation = static_cast<dx::extension::UWindow*>(newChild(root, "DeusEx.ConWindowActive"));
    REQUIRE(conversation != nullptr);
    {
        dx::vm::Call call(conversation, dx::Name("SetWindowAlignments"));
        call.byte(3).byte(3);
        call.run();
    }
    *reinterpret_cast<float*>(conversation->propertyAddress(dx::Name("currentWindowPos"))) = 1.0f;
    layout(1720, 720);
    voidCall(conversation, "CalculateWindowSizes");
    CHECK(conversation->width() == 1280.0f);

    dx::UObject* texture = *reinterpret_cast<dx::UObject**>(conversation->propertyAddress(dx::Name("conTexture")));
    REQUIRE(texture != nullptr);
    RecordingCanvas canvas(1720, 720);
    dx::extension::render(canvas);
    int bars = 0;
    for (const RecordedTile& tile : canvas.tiles) {
        if (tile.texture == texture) {
            CHECK(tile.x == 0.0f);
            CHECK(tile.w == 1720.0f);
            bars++;
        }
    }
    CHECK(bars == 2);
    dx::extension::setViewportSize(800, 600);
}

TEST_CASE("conversation choices wrap, scroll to focus and revert in the open conversation", "[data]")
{
    auto* root = makeRoot("DeusEx.DeusExRootWindow");
    voidCall(root, "DestroyAllChildren");
    auto& ini = dx::config::system();
    auto previous = ini.get("WinDrv.WindowsClient", "CenteredConversations").value_or("True");
    dx::test::atTestEnd([previous] {
        dx::console::execute(nullptr, "set ini:Engine.Engine.ViewportManager CenteredConversations " + previous);
        dx::extension::setUIScale(0);
        dx::extension::setViewportSize(800, 600);
    });
    ini.set("WinDrv.WindowsClient", "CenteredConversations", "True");
    dx::extension::setUIScale(4);
    auto* screen = dx::cast<dx::extension::UModalWindow>(newChild(root, "DeusEx.ConWindowActive"));
    REQUIRE(screen);
    dx::vm::Call(screen, dx::Name("SetWindowAlignments")).byte(3).byte(3).run();
    *dx::script::field<float>(screen, "currentWindowPos") = 1;
    auto* panel = dx::cast<dx::extension::UWindow>(dx::script::object(screen, "lowerConWindow"));
    REQUIRE(panel);
    auto* speech = newChild(panel, "DeusEx.ConWindowSpeech");
    dx::vm::Call(speech, dx::Name("SetName")).s("Paul").run();
    dx::vm::Call(speech, dx::Name("SetSpeech")).s("Take a look.").run();
    layout(3840, 2160);
    auto* line = dx::cast<dx::extension::UWindow>(speech);
    CHECK(line->x() > panel->width() / 4);
    CHECK(std::abs(line->x() * 2 + line->width() - panel->width()) <= 1);
    float shortHeight = line->height();
    dx::vm::Call(speech, dx::Name("SetSpeech")).s("Ha!  They're monkeys!  That's the thing: maybe they have super-brains, but "
                                                  "they don't know anything about any of the things we take for granted.  A "
                                                  "gun is as much a part of their world as a car is to us, and a fire is as "
                                                  "mysterious as the sun.  They can learn, but slowly, and they can't be "
                                                  "trusted with anything we depend on.").run();
    layout(3840, 2160);
    CHECK(line->height() > shortHeight);
    CHECK(line->width() <= panel->width() - 12);
    CHECK(line->x() >= 0);
    CHECK(line->x() + line->width() <= panel->width());
    std::vector<dx::extension::UButtonWindow*> replies;
    for (int i = 0; i < 9; ++i) {
        auto* reply = dx::cast<dx::extension::UButtonWindow>(newChild(panel, "DeusEx.ConChoiceWindow"));
        setText(reply, "~ That's more than I can spend on an upgrade right now. What else do you have for sale?");
        replies.push_back(reply);
    }
    layout(1280, 720);
    auto* last = replies.back();
    auto* rootWindow = screen->root();
    float letterboxHeight = panel->height();
    float renderY = dx::rootwindow::renderY(rootWindow);
    float renderHeight = dx::rootwindow::renderHeight(rootWindow);
    CHECK(screen->scrollContent(0));
    CHECK(last->y() >= panel->height());
    REQUIRE(screen->setFocusWindow(last));
    layout(1280, 720);
    CHECK(last->y() >= 0);
    CHECK(last->y() + last->height() <= panel->height());
    CHECK(last->x() == replies.front()->x());
    CHECK(last->displayedText() == last->text().substr(2));
    CHECK(panel->height() == letterboxHeight);
    CHECK(dx::rootwindow::renderY(rootWindow) == renderY);
    CHECK(dx::rootwindow::renderHeight(rootWindow) == renderHeight);

    // The page keys, the mouse wheel and the pad's scroll stick all move the same text.
    REQUIRE(screen->setFocusWindow(replies.front()));
    layout(1280, 720);
    float topY = last->y();
    CHECK(dx::extension::keyEvent(dx::IK_PageDown, dx::IST_Press, 0));
    dx::extension::keyEvent(dx::IK_PageDown, dx::IST_Release, 0);
    layout(1280, 720);
    float pageY = last->y();
    CHECK(pageY < topY);
    CHECK(dx::extension::keyEvent(dx::IK_MouseWheelUp, dx::IST_Press, 0));
    dx::extension::keyEvent(dx::IK_MouseWheelUp, dx::IST_Release, 0);
    layout(1280, 720);
    CHECK(last->y() > pageY);
    CHECK(dx::extension::scroll(false));
    layout(1280, 720);
    CHECK(last->y() < topY);

    // A slim scrollbar in the letterbox says there is more text than fits.
    RecordingCanvas frame(1280, 720);
    dx::extension::render(frame);
    float panelX = 0;
    float panelY = 0;
    panel->absolutePosition(panelX, panelY);
    float scale = dx::extension::uiScale();
    float barX = (panelX + panel->width() - 5) * scale;
    float barTop = (panelY + 2) * scale;
    float barBottom = (panelY + panel->height()) * scale;
    bool scrollbar = false;
    for (const RecordedRect& rect : frame.rectangles) {
        if (std::abs(rect.x - barX) <= 1.5f * scale && rect.w <= 4.0f * scale && rect.h >= 8.0f * scale &&
            rect.y >= barTop && rect.y + rect.h <= barBottom) {
            scrollbar = true;
            break;
        }
    }
    CHECK(scrollbar);

    CHECK(dx::console::execute(nullptr, "set ini:Engine.Engine.ViewportManager CenteredConversations False").empty());
    screen->tick(0);
    layout(1280, 720);
    CHECK_FALSE(screen->scrollContent(0));
    CHECK(last->displayedText() == last->text());
    CHECK(panel->height() == letterboxHeight);
    CHECK(dx::rootwindow::renderY(rootWindow) == renderY);
    CHECK(dx::rootwindow::renderHeight(rootWindow) == renderHeight);
    CHECK(line->x() <= 2);
}

TEST_CASE("Scope view borders drawn by script cover an ultrawide screen around a centered scope", "[data]")
{
    initializeEngine();
    dx::extension::reset();
    dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* root = dx::extension::rootWindow();
        REQUIRE(root != nullptr);
        REQUIRE(root->cls->name == dx::Name("DeusExRootWindow"));
        dx::UObject* scope = *reinterpret_cast<dx::UObject**>(root->propertyAddress(dx::Name("scopeView")));
        REQUIRE(scope != nullptr);
        dx::extension::setViewportSize(3360, 1440);
        layout(3360, 1440);
        {
            dx::vm::Call call(scope, dx::Name("ActivateView"));
            call.i(30).b(false).b(true);
            call.run();
        }
        RecordingCanvas canvas(3360, 1440);
        dx::extension::render(canvas);
        auto drawn = [&canvas](float x, float y, float w, float h) {
            for (const RecordedTile& tile : canvas.tiles) {
                if (tile.x == x && tile.y == y && tile.w == w && tile.h == h) {
                    return true;
                }
            }
            return false;
        };
        CHECK(drawn(0, 0, 3360, 464));
        CHECK(drawn(0, 976, 3360, 464));
        CHECK(drawn(0, 464, 1424, 512));
        CHECK(drawn(1936, 464, 1424, 512));
        CHECK(drawn(1424, 464, 512, 512));
        dx::extension::setViewportSize(800, 600);
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("ResetUnitHeight tests the vertical units but resets the horizontal ones, as the original does", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* clip = newChild(root, "Extension.ClipWindow");
    dx::clipwindow::prefHUnits(clip) = 3;
    dx::clipwindow::prefVUnits(clip) = 4;
    voidCall(clip, "ResetUnitHeight");
    CHECK(dx::clipwindow::prefHUnits(clip) == -1);
    CHECK(dx::clipwindow::prefVUnits(clip) == 4);
    dx::clipwindow::prefHUnits(clip) = 3;
    dx::clipwindow::prefVUnits(clip) = -1;
    voidCall(clip, "ResetUnitHeight");
    CHECK(dx::clipwindow::prefHUnits(clip) == 3);
    voidCall(clip, "ResetUnitWidth");
    CHECK(dx::clipwindow::prefHUnits(clip) == -1);
}

TEST_CASE("Scroll area preferred height uses the margin width, as the original does", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* area = newChild(root, "Extension.ScrollAreaWindow");
    auto preferred = [area](float& w, float& h) {
        dx::vm::Call call(area, dx::Name("QueryPreferredSize"));
        call.f(0).f(0);
        call.run();
        w = *reinterpret_cast<float*>(call.parameter(0));
        h = *reinterpret_cast<float*>(call.parameter(1));
    };
    dx::scrollareawindow::marginWidth(area) = 3.0f;
    dx::scrollareawindow::marginHeight(area) = 3.0f;
    float w0 = 0, h0 = 0;
    preferred(w0, h0);
    dx::scrollareawindow::marginHeight(area) = 10.0f;
    float w1 = 0, h1 = 0;
    preferred(w1, h1);
    CHECK(w1 == w0);
    CHECK(h1 == h0);
    dx::scrollareawindow::marginWidth(area) = 10.0f;
    float w2 = 0, h2 = 0;
    preferred(w2, h2);
    CHECK(w2 == w0 + 14.0f);
    CHECK(h2 == h0 + 14.0f);
}

TEST_CASE("Computer window buffer counts as flushed only while its event queue is empty", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* computer = newChild(root, "Extension.ComputerWindow");
    dx::vm::Call empty(computer, dx::Name("IsBufferFlushed"));
    CHECK(boolResult(empty));
    {
        dx::vm::Call call(computer, dx::Name("Print"));
        call.s("abc").b(false);
        call.run();
    }
    dx::vm::Call before(computer, dx::Name("IsBufferFlushed"));
    CHECK_FALSE(boolResult(before));
    dx::extension::tick(1.0f);
    dx::extension::tick(1.0f);
    // XComputerWindow::IsBufferFlushed ignores how much of the
    // queue has been processed: printed events stay queued.
    auto* window = static_cast<dx::extension::UComputerWindow*>(computer);
    CHECK(window->queuedStart == window->queued.size());
    dx::vm::Call after(computer, dx::Name("IsBufferFlushed"));
    CHECK_FALSE(boolResult(after));
}

TEST_CASE("EnableSpecialText controls metachar parsing and is saved by PushGC", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* window = newChild(root, "Extension.Window");
    dx::UObject* gc = objectCall(window, "GetGC");
    REQUIRE(gc != nullptr);
    auto enable = [gc](bool value) {
        dx::vm::Call call(gc, dx::Name("EnableSpecialText"));
        call.b(value);
        call.run();
    };
    auto enabled = [gc]() {
        dx::vm::Call call(gc, dx::Name("IsSpecialTextEnabled"));
        return boolResult(call);
    };
    enable(true);
    CHECK(dx::gc::bParseMetachars(gc) == 1);
    CHECK(enabled());
    {
        dx::vm::Call push(gc, dx::Name("PushGC"));
        push.run();
    }
    enable(false);
    CHECK(dx::gc::bParseMetachars(gc) == 0);
    CHECK_FALSE(enabled());
    {
        dx::vm::Call pop(gc, dx::Name("PopGC"));
        pop.skip();
        pop.run();
    }
    CHECK(enabled());
    dx::vm::Call release(window, dx::Name("ReleaseGC"));
    release.obj(gc);
    release.run();
}

namespace {
    std::pair<std::string, int32_t> textPart(dx::UObject* object, int32_t start, int32_t count)
    {
        dx::vm::Call call(object, dx::Name("GetTextPart"));
        REQUIRE(call.valid());
        call.i(start).i(count).s("");
        call.run();
        return { *reinterpret_cast<std::string*>(call.parameter(2)), *reinterpret_cast<int32_t*>(call.returnValue()) };
    }
}

TEST_CASE("TextWindow GetTextPart clamps like FString::Mid in unsigned 32 bits", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* text = newChild(root, "Extension.TextWindow");
    setText(text, "Hello World");
    using Part = std::pair<std::string, int32_t>;
    CHECK(textPart(text, 6, 2147483647) == Part { "World", 5 });
    CHECK(textPart(text, 0, 5) == Part { "Hello", 5 });
    CHECK(textPart(text, -1, 5) == Part { "", 0 });
    // 2 + -5 wraps to a huge unsigned end, which clamps to the length.
    CHECK(textPart(text, 2, -5) == Part { "llo World", 9 });
    CHECK(textPart(text, 4, -2) == Part { "", 0 });
    CHECK(textPart(text, 20, 3) == Part { "", 0 });
}

TEST_CASE("ExtString GetTextPart caps the count without adding it to the start", "[data]")
{
    initializeEngine();
    dx::UObject* ext = dx::objects::newObject(dx::objects::findClass("Extension.ExtString"), dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
    REQUIRE(ext != nullptr);
    setText(ext, "Hello World");
    using Part = std::pair<std::string, int32_t>;
    CHECK(textPart(ext, 6, 2147483647) == Part { "World", 5 });
    CHECK(textPart(ext, -1, 5) == Part { "", 0 });
    CHECK(textPart(ext, 2, -5) == Part { "", 0 });
    CHECK(textPart(ext, 20, 3) == Part { "", 0 });
    CHECK(textPart(ext, -2147483647 - 1, 5) == Part { "", 0 });
    setText(ext, std::string(300, 'x'));
    CHECK(textPart(ext, 10, 2147483647) == Part { std::string(240, 'x'), 240 });
}

TEST_CASE("EditWindow SetSelectedArea wraps start plus count and clamps the end to the text", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* edit = newChild(root, "Extension.EditWindow");
    setText(edit, "Hello World");
    auto select = [edit](int32_t start, int32_t count) {
        dx::vm::Call call(edit, dx::Name("SetSelectedArea"));
        call.i(start).i(count);
        call.run();
        return std::pair { dx::editwindow::insertHookPos(edit), dx::editwindow::insertPos(edit) };
    };
    CHECK(select(2, 3) == std::pair { 2, 5 });
    // 5 + MaxInt wraps negative, so the caret goes to 0 and selects 0..5.
    CHECK(select(5, 2147483647) == std::pair { 5, 0 });
    CHECK(dx::editwindow::selectStart(edit) == 0);
    CHECK(dx::editwindow::selectEnd(edit) == 5);
    CHECK(select(3, 100) == std::pair { 3, 11 });
}

namespace {
    std::vector<std::pair<std::string, dx::UObject*>> windowEvents;
    bool destroyInInit = false;

    // Records the events NewChild sends, without running the class's script.
    class RecordingWindow : public dx::extension::UWindow {
    public:
        void initWindow() override
        {
            windowEvents.push_back({ "InitWindow", this });
            if (destroyInInit) {
                destroyWindowTree();
            }
        }

        void childAdded(dx::extension::UWindow* child) override
        {
            windowEvents.push_back({ "ChildAdded", this });
            (void)child;
        }

        void descendantAdded(dx::extension::UWindow* descendant) override
        {
            windowEvents.push_back({ "DescendantAdded", this });
            (void)descendant;
        }

        // Records the clip it is drawn with and, if asked, pushes the GC and
        // narrows the clip without popping, as an unbalanced script would.
        void drawWindow(dx::extension::UGC* gc) override
        {
            drawnGC = gc;
            drawnClipWidths.push_back(gc->clipRight - gc->clipLeft);
            if (leakGC) {
                dx::vm::Call push(gc, dx::Name("PushGC"));
                push.run();
                dx::vm::Call cut(gc, dx::Name("Intersect"));
                cut.f(0).f(0).f(1).f(1);
                cut.run();
            }
        }

        static inline std::vector<float> drawnClipWidths;
        static inline bool leakGC = false;
        static inline dx::extension::UGC* drawnGC = nullptr;
    };

    struct RecordingFactory {
        RecordingFactory()
        {
            for (const char* name : { "HUDBaseWindow", "PersonaBaseWindow" }) {
                dx::UClass* cls = dx::objects::findClass((std::string("DeusEx.") + name).c_str());
                REQUIRE(cls != nullptr);
                REQUIRE(cls->superClass()->name == dx::Name("Window"));
                dx::objects::registerFactory(dx::Name(name), [] { return new RecordingWindow(); });
            }
        }

        ~RecordingFactory()
        {
            // With no factory of their own, both classes get Window's.
            for (const char* name : { "HUDBaseWindow", "PersonaBaseWindow" }) {
                dx::objects::registerFactory(dx::Name(name), [] { return new dx::extension::UWindow(); });
            }
            windowEvents.clear();
            destroyInInit = false;
            RecordingWindow::drawnClipWidths.clear();
            RecordingWindow::leakGC = false;
        }
    };
}

TEST_CASE("A PushGC that a window's drawing leaves unpopped ends with that window", "[data]")
{
    dx::UObject* root = makeRoot();
    RecordingFactory factory;
    auto* rootWindow = static_cast<dx::extension::UWindow*>(root);
    dx::extension::UWindow* first = rootWindow->newChild(dx::objects::findClass("DeusEx.HUDBaseWindow"), true);
    dx::extension::UWindow* second = rootWindow->newChild(dx::objects::findClass("DeusEx.HUDBaseWindow"), true);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    setSize(first, 100, 100);
    setPos(first, 0, 0);
    setSize(second, 100, 100);
    setPos(second, 200, 0);
    RecordingWindow::leakGC = true;
    for (int frame = 0; frame < 3; frame++) {
        RecordingWindow::drawnClipWidths.clear();
        layout(800, 600);
        REQUIRE(RecordingWindow::drawnClipWidths.size() == 2);
        CHECK(RecordingWindow::drawnClipWidths[0] == 100.0f);
        CHECK(RecordingWindow::drawnClipWidths[1] == 100.0f);
        REQUIRE(RecordingWindow::drawnGC != nullptr);
        CHECK(RecordingWindow::drawnGC->savedStates.empty());
    }
}

TEST_CASE("NewChild sends ChildAdded and DescendantAdded to the parent and every ancestor before InitWindow", "[data]")
{
    dx::UObject* root = makeRoot();
    RecordingFactory factory;
    auto* rootWindow = static_cast<dx::extension::UWindow*>(root);
    dx::extension::UWindow* grandparent = rootWindow->newChild(dx::objects::findClass("DeusEx.HUDBaseWindow"), true);
    REQUIRE(grandparent != nullptr);
    dx::extension::UWindow* parent = grandparent->newChild(dx::objects::findClass("DeusEx.HUDBaseWindow"), true);
    REQUIRE(parent != nullptr);
    windowEvents.clear();
    dx::extension::UWindow* child = parent->newChild(dx::objects::findClass("DeusEx.PersonaBaseWindow"), true);
    REQUIRE(child != nullptr);
    std::vector<std::pair<std::string, dx::UObject*>> expected {
        { "ChildAdded", parent },
        { "DescendantAdded", parent },
        { "DescendantAdded", grandparent },
        { "InitWindow", child },
    };
    CHECK(windowEvents == expected);
    CHECK(child->visible());
}

TEST_CASE("NewChild returns None for a child that its own InitWindow destroyed", "[data]")
{
    dx::UObject* root = makeRoot();
    RecordingFactory factory;
    auto* rootWindow = static_cast<dx::extension::UWindow*>(root);
    dx::extension::UWindow* before = rootWindow->lastChild();
    destroyInInit = true;
    CHECK(rootWindow->newChild(dx::objects::findClass("DeusEx.PersonaBaseWindow"), true) == nullptr);
    CHECK(rootWindow->lastChild() == before);
    destroyInInit = false;
    dx::UObject* script = newChild(root, "DeusEx.PersonaBaseWindow");
    CHECK(script != nullptr);
}

TEST_CASE("A scroll area with nothing to scroll leaves the mouse wheel to its parent", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* area = newChild(root, "Extension.ScrollAreaWindow");
    layout(800, 600);
    auto* window = static_cast<dx::extension::UWindow*>(area);
    auto* vScale = static_cast<dx::extension::UScaleWindow*>(dx::scrollareawindow::vScale(area));
    REQUIRE(vScale != nullptr);
    vScale->setRanges(10, 10);
    CHECK_FALSE(window->mouseButtonPressed(0, 0, dx::IK_MouseWheelDown, 1));
    vScale->setRanges(10, 100);
    REQUIRE(vScale->maxPosition() > 0);
    int32_t before = dx::scalewindow::currentPos(vScale);
    CHECK(window->mouseButtonPressed(0, 0, dx::IK_MouseWheelDown, 1));
    CHECK(dx::scalewindow::currentPos(vScale) > before);
}

TEST_CASE("List focus and anchor rows follow their row through DeleteRow and ResetSortColumns", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* object = newChild(root, "Extension.ListWindow");
    auto* list = static_cast<dx::extension::UListWindow*>(object);
    for (const char* text : { "a", "b", "c", "d", "e" }) {
        dx::vm::Call call(object, dx::Name("AddRow"));
        call.s(text).i(0);
        call.run();
    }
    auto field = [&](int32_t index) {
        return list->rows[static_cast<size_t>(index)].fields[0];
    };
    auto deleteRow = [&](int32_t index) {
        dx::vm::Call call(object, dx::Name("DeleteRow"));
        call.i(list->rows[static_cast<size_t>(index)].id);
        call.run();
    };
    int32_t& focus = dx::listwindow::focusLine(object);
    int32_t& anchor = dx::listwindow::anchorLine(object);
    focus = 2;
    anchor = 3;
    // A row above both moves them up with their rows.
    deleteRow(0);
    CHECK(focus == 1);
    CHECK(field(focus) == "c");
    CHECK(anchor == 2);
    CHECK(field(anchor) == "d");
    // Deleting the focus row leaves no focus row.
    deleteRow(1);
    CHECK(focus == -1);
    CHECK(field(anchor) == "d");
    // Re-sorting keeps them on their rows.
    dx::extension::UListWindow::SortKey key;
    key.column = 0;
    key.reverse = true;
    list->sortKeys.assign(1, key);
    list->sortRows();
    CHECK(field(0) == "e");
    focus = 0;
    {
        dx::vm::Call call(object, dx::Name("ResetSortColumns"));
        call.b(true);
        call.run();
    }
    CHECK(field(0) == "b");
    CHECK(field(focus) == "e");
    CHECK(field(anchor) == "d");
}

TEST_CASE("Huge script indices are ignored instead of growing list and scale arrays", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* object = newChild(root, "Extension.ListWindow");
    auto* list = static_cast<dx::extension::UListWindow*>(object);
    int32_t rowId = 0;
    {
        dx::vm::Call call(object, dx::Name("AddRow"));
        call.s("a").i(0);
        call.run();
        rowId = list->rows[0].id;
    }
    auto setField = [&](int32_t column, const char* text) {
        dx::vm::Call call(object, dx::Name("SetField"));
        call.i(rowId).i(column).s(text);
        call.run();
    };
    // XListWindow::Init starts with one column, so column 0 is valid.
    setField(0, "b");
    CHECK(list->rows[0].fields[0] == "b");
    setField(2000000000, "c");
    CHECK(list->rows[0].fields.size() == 1);
    {
        dx::vm::Call call(object, dx::Name("SetNumColumns"));
        call.i(2000000000);
        call.run();
    }
    CHECK(list->columns.size() < 2);
    {
        dx::vm::Call call(object, dx::Name("SetNumColumns"));
        call.i(3);
        call.run();
    }
    setField(2, "c");
    CHECK(list->rows[0].fields[2] == "c");

    dx::UObject* scaleObject = newChild(root, "Extension.ScaleWindow");
    auto* scale = static_cast<dx::extension::UScaleWindow*>(scaleObject);
    auto setEnumeration = [&](int32_t position, const char* text) {
        dx::vm::Call call(scaleObject, dx::Name("SetEnumeration"));
        call.i(position).s(text);
        call.run();
    };
    setEnumeration(0x200, "x");
    setEnumeration(2000000000, "x");
    CHECK(scale->enumerations.empty());
    setEnumeration(0x1ff, "y");
    REQUIRE(scale->enumerations.size() == 0x200);
    CHECK(scale->enumerations[0x1ff] == "y");
}

TEST_CASE("Flag iterators skip deleted flags and end after DeleteAllFlags", "[data]")
{
    initializeEngine();
    dx::UObject* base = dx::objects::newObject(dx::objects::findClass("Extension.FlagBase"), dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
    // More flags than hash buckets, so some buckets hold several.
    for (int i = 0; i < 200; i++) {
        dx::vm::Call call(base, dx::Name("SetBool"));
        call.n(dx::Name("Flag" + std::to_string(i))).b(true).b(true);
        call.run();
    }
    auto create = [base]() {
        dx::vm::Call call(base, dx::Name("CreateIterator"));
        call.byte(0);
        call.run();
        return *reinterpret_cast<int32_t*>(call.returnValue());
    };
    auto next = [base](int32_t iterator, dx::Name& name) {
        dx::vm::Call call(base, dx::Name("GetNextFlagName"));
        call.i(iterator).n(dx::Name());
        bool ok = boolResult(call);
        name = *reinterpret_cast<dx::Name*>(call.parameter(1));
        return ok;
    };
    auto remove = [base](dx::Name name) {
        dx::vm::Call call(base, dx::Name("DeleteFlag"));
        call.n(name).byte(0);
        call.run();
    };
    std::vector<dx::Name> order;
    {
        int32_t iterator = create();
        dx::Name name;
        while (next(iterator, name)) {
            order.push_back(name);
        }
    }
    REQUIRE(order.size() == 200);
    std::vector<dx::Name> expected;
    std::vector<dx::Name> seen;
    for (size_t i = 0; i < order.size(); i++) {
        if (i != 1 && i != 3) {
            expected.push_back(order[i]);
        }
    }
    int32_t iterator = create();
    dx::Name name;
    while (next(iterator, name)) {
        seen.push_back(name);
        if (seen.size() == 1) {
            // Not reached yet, whether or not it is the iterator's next flag.
            remove(order[1]);
            remove(order[3]);
        } else if (seen.size() == 2) {
            // The flag just returned.
            remove(name);
        }
    }
    CHECK(seen == expected);

    int32_t cleared = create();
    REQUIRE(next(cleared, name));
    voidCall(base, "DeleteAllFlags");
    CHECK_FALSE(next(cleared, name));
}

TEST_CASE("Backspace in terminal input drops an echo that has not been placed yet", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* object = newChild(root, "Extension.ComputerWindow");
    auto* computer = static_cast<dx::extension::UComputerWindow*>(object);
    computer->inputActive = true;
    computer->inputMax = 0;
    auto shown = [computer](char c) {
        for (const auto& element : computer->display) {
            if (element.type == dx::extension::UComputerWindow::EventCharacter && element.ch == c) {
                return true;
            }
        }
        return false;
    };
    dx::extension::tick(1.0f);
    dx::extension::tick(1.0f);
    REQUIRE(computer->keyPressed("a"));
    CHECK(dx::computerwindow::TextX(object) == 0);
    REQUIRE(computer->virtualKeyPressed(dx::IK_Backspace, false));
    CHECK(computer->input.empty());
    CHECK(dx::computerwindow::TextX(object) == 0);
    dx::extension::tick(1.0f);
    dx::extension::tick(1.0f);
    CHECK_FALSE(shown('a'));
    CHECK(dx::computerwindow::TextX(object) == 0);

    // Once placed, Backspace takes the character off the screen.
    REQUIRE(computer->keyPressed("b"));
    dx::extension::tick(1.0f);
    dx::extension::tick(1.0f);
    CHECK(shown('b'));
    CHECK(dx::computerwindow::TextX(object) == 1);
    REQUIRE(computer->virtualKeyPressed(dx::IK_Backspace, false));
    CHECK_FALSE(shown('b'));
    CHECK(dx::computerwindow::TextX(object) == 0);
}

TEST_CASE("a system keyboard edits only a text field the player can type in", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* label = newChild(root, "Extension.TextWindow");
    dx::UObject* edit = newChild(root, "Extension.EditWindow");
    setText(edit, "Gr\xfc\xdf");
    CHECK_FALSE(dx::textentry::requestFor(nullptr));
    CHECK_FALSE(dx::textentry::requestFor(label));
    std::optional<dx::textentry::Request> request = dx::textentry::requestFor(edit);
    REQUIRE(request);
    // The game's Windows-1252 text goes to the keyboard as UTF-8.
    CHECK(request->text == "Gr\xc3\xbc\xc3\x9f");
    CHECK_FALSE(request->password);
    dx::editwindow::bEditable(edit) = 0;
    CHECK_FALSE(dx::textentry::requestFor(edit));
    dx::editwindow::bEditable(edit) = 1;
    {
        dx::vm::Call call(edit, dx::Name("SetSensitivity"));
        call.b(false);
        call.run();
    }
    CHECK_FALSE(dx::textentry::requestFor(edit));
}

TEST_CASE("what is typed on a system keyboard replaces the field's text through its filter and limit", "[data]")
{
    dx::UObject* root = makeRoot("DeusEx.DeusExRootWindow");
    dx::UObject* modal = newChild(root, "Extension.ModalWindow");
    dx::UObject* edit = newChild(modal, "DeusEx.MenuUIEditWindow");
    setText(edit, "New Save Game");
    {
        dx::vm::Call call(edit, dx::Name("SetMaxSize"));
        call.i(8);
        call.run();
    }
    {
        dx::vm::Call call(edit, dx::Name("EnableSingleLineEditing"));
        call.b(true);
        call.run();
    }
    std::optional<dx::textentry::Request> request = dx::textentry::requestFor(edit);
    REQUIRE(request);
    CHECK(request->maxLength == 8);
    CHECK_FALSE(request->multiline);
    dx::textentry::deliver(*request, "Liberty\nIsland \xe2\x82\xac");
    // The newline is dropped from a one-line field and the rest cut to its eight characters.
    CHECK(dx::textwindow::Text(edit) == "LibertyI");
    {
        dx::vm::Call call(edit, dx::Name("SetFilter"));
        call.s("abc");
        call.run();
    }
    dx::textentry::deliver(*request, "xyz");
    // Text the filter refuses entirely leaves the field as it was.
    CHECK(dx::textwindow::Text(edit) == "LibertyI");
    dx::textentry::deliver(*request, "cab");
    CHECK(dx::textwindow::Text(edit) == "cab");
}

TEST_CASE("the computer login's password and the ATM's PIN use the keyboard's password mode", "[data]")
{
    dx::UObject* root = makeRoot("DeusEx.DeusExRootWindow");
    for (auto [screenClass, nameField, secretField] : { std::tuple { "DeusEx.ComputerScreenLogin", "editUserName", "editPassword" }, std::tuple { "DeusEx.ComputerScreenATM", "editAccount", "editPIN" } }) {
        dx::UObject* screen = newChild(root, screenClass);
        REQUIRE(screen != nullptr);
        dx::UObject* name = dx::script::object(screen, nameField);
        dx::UObject* secret = dx::script::object(screen, secretField);
        REQUIRE(name != nullptr);
        REQUIRE(secret != nullptr);
        CHECK_FALSE(dx::textentry::requestFor(name)->password);
        CHECK(dx::textentry::requestFor(secret)->password);
    }
}

TEST_CASE("GC DrawActor draws the actor in the GC's style, unlit, scaled and glowing, then restores it", "[data]")
{
    initializeEngine();
    dx::UClass* actorClass = dx::objects::findClass("Engine.Actor");
    REQUIRE(actorClass != nullptr);
    dx::UObject* actor = dx::objects::newObject(actorClass, dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
    dx::actor::Style(actor) = 1;
    dx::actor::bUnlit(actor) = 0;
    dx::actor::DrawScale(actor) = 1.5f;
    dx::actor::ScaleGlow(actor) = 0.5f;
    dx::extension::ActorDrawRequest request;
    request.actor = actor;
    request.unlit = true;
    request.drawScale = 2.0f;
    request.scaleGlow = 2.0f;
    bool drawn = false;
    // DSTY_Translucent, as the vision augmentation and the actor display's mesh view set it.
    dx::extension::drawActorAs(request, 3, [&] {
        drawn = true;
        CHECK(dx::actor::Style(actor) == 3);
        CHECK(dx::actor::bUnlit(actor) == 1);
        CHECK(dx::actor::DrawScale(actor) == 3.0f);
        CHECK(dx::actor::ScaleGlow(actor) == 2.0f);
    });
    CHECK(drawn);
    CHECK(dx::actor::Style(actor) == 1);
    CHECK(dx::actor::bUnlit(actor) == 0);
    CHECK(dx::actor::DrawScale(actor) == 1.5f);
    CHECK(dx::actor::ScaleGlow(actor) == 0.5f);
}

TEST_CASE("The root window draws window frames and statistics when ShowFrames and ShowStats turn them on", "[data]")
{
    dx::UObject* root = makeRoot();
    dx::UObject* window = newChild(root, "Extension.Window");
    setPos(window, 10, 10);
    setSize(window, 100, 50);
    auto rectsDrawn = [] {
        RecordingCanvas canvas(640, 480);
        dx::extension::render(canvas);
        return canvas.rects;
    };
    int plain = rectsDrawn();
    dx::vm::Call frames(root, dx::Name("ShowFrames"));
    REQUIRE(frames.valid());
    frames.b(true);
    frames.run();
    CHECK(dx::rootwindow::bShowFrames(root) == 1);
    // Four sides round the root and four round the child.
    CHECK(rectsDrawn() == plain + 8);
    dx::vm::Call framesOff(root, dx::Name("ShowFrames"));
    framesOff.b(false);
    framesOff.run();
    CHECK(rectsDrawn() == plain);
    dx::vm::Call stats(root, dx::Name("ShowStats"));
    REQUIRE(stats.valid());
    stats.b(true);
    stats.run();
    // A dark band behind each of the four lines.
    CHECK(rectsDrawn() == plain + 4);
}

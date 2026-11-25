#include "core/config.h"
#include "test_paths.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/hud.h"
#include "deusex/gamepad_layout.h"
#include "deusex/glyphs.h"
#include "deusex/script.h"
#include "deusex/startup.h"
#include "engine/console.h"
#include "engine/gamepad.h"
#include "engine/input.h"
#include "engine/resources.h"
#include "engine/props_engine.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "game/gamepad_input.h"
#include "layout_row.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>
#include <vector>

namespace {
    SDL_JoystickID attachPad(Uint16 vendor = 0, Uint16 product = 0)
    {
        SDL_VirtualJoystickDesc description {};
        description.version = sizeof(description);
        description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        description.vendor_id = vendor;
        description.product_id = product;
        description.naxes = SDL_GAMEPAD_AXIS_COUNT;
        description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        return SDL_AttachVirtualJoystick(&description);
    }

    void padEvent(dx::game::GamepadInput& pad, Uint32 type, SDL_JoystickID id, int which = 0, int16_t value = 0)
    {
        SDL_Event event {};
        event.type = type;
        if (type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            event.gbutton.which = id;
            event.gbutton.button = static_cast<uint8_t>(which);
        } else if (type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            event.gaxis.which = id;
            event.gaxis.axis = static_cast<uint8_t>(which);
            event.gaxis.value = value;
        } else {
            event.gdevice.which = id;
        }
        pad.handleEvent(event);
    }
}

TEST_CASE("sticks ignore the dead zone and rescale the rest of their travel", "[gamepad]")
{
    dx::gamepad::Stick inside = dx::gamepad::shapeStick(0.1f, 0.1f, 0.2f, 1.0f);
    CHECK(inside.x == 0.0f);
    CHECK(inside.y == 0.0f);
    dx::gamepad::Stick full = dx::gamepad::shapeStick(0.0f, -1.0f, 0.2f, 1.0f);
    CHECK(full.x == 0.0f);
    CHECK(full.y == Catch::Approx(-1.0f));
    dx::gamepad::Stick halfway = dx::gamepad::shapeStick(0.6f, 0.0f, 0.2f, 1.0f);
    CHECK(halfway.x == Catch::Approx(0.5f));
    dx::gamepad::Stick curved = dx::gamepad::shapeStick(0.6f, 0.0f, 0.2f, 2.0f);
    CHECK(curved.x == Catch::Approx(0.25f));
}

TEST_CASE("sticks keep their direction and clamp diagonals to unit length", "[gamepad]")
{
    dx::gamepad::Stick corner = dx::gamepad::shapeStick(1.0f, 1.0f, 0.15f, 2.0f);
    CHECK(std::sqrt(corner.x * corner.x + corner.y * corner.y) == Catch::Approx(1.0f));
    CHECK(corner.x == Catch::Approx(corner.y));
}

TEST_CASE("a stick counts as all the way out past its outer zone", "[gamepad]")
{
    dx::gamepad::Stick outer = dx::gamepad::shapeStick(0.9f, 0.0f, 0.1f, 1.0f, 0.9f);
    CHECK(outer.x == Catch::Approx(1.0f));
    dx::gamepad::Stick halfway = dx::gamepad::shapeStick(0.5f, 0.0f, 0.1f, 1.0f, 0.9f);
    CHECK(halfway.x == Catch::Approx(0.5f));
    // A dead zone reaching the outer zone leaves nothing.
    CHECK(dx::gamepad::shapeStick(0.95f, 0.0f, 0.9f, 1.0f, 0.9f).x == 0.0f);
}

TEST_CASE("flick stick turns to face the way the stick points, then follows it about the rim", "[gamepad]")
{
    constexpr float pi = 3.14159265f;
    dx::gamepad::FlickStick flick;
    // Below the rim it does nothing.
    CHECK(flick.update(0.5f, 0.0f, 0.01f) == 0.0f);
    // Pushed right, it turns a quarter turn right over flickSeconds, starting at once.
    float turned = flick.update(1.0f, 0.0f, 0.01f);
    CHECK(turned > 0.0f);
    for (int i = 0; i < 20; i++) {
        turned += flick.update(1.0f, 0.0f, 0.01f);
    }
    CHECK(turned == Catch::Approx(pi * 0.5f).margin(1e-4));
    // Turning the stick on to straight down turns as far again, paid out evenly, all of it
    // once the stick has stopped a moment.
    float rotated = 0;
    for (int i = 1; i <= 10; i++) {
        float angle = pi * 0.5f + pi * 0.5f * static_cast<float>(i) / 10.0f;
        rotated += flick.update(std::sin(angle), -std::cos(angle), 0.01f);
    }
    CHECK(rotated > pi * 0.4f);
    for (int i = 0; i < 20; i++) {
        rotated += flick.update(0.0f, 1.0f, 0.01f);
    }
    CHECK(rotated == Catch::Approx(pi * 0.5f).margin(1e-4));
    // Let go, then a push behind flicks round by half a turn, either way.
    flick.update(0.0f, 0.0f, 0.01f);
    float behind = 0;
    for (int i = 0; i < 20; i++) {
        behind += flick.update(0.0f, 1.0f, 0.01f);
    }
    CHECK(std::abs(behind) == Catch::Approx(pi).margin(1e-4));
}

TEST_CASE("a steady turn about the rim turns the view evenly however the stick's reports fall on frames", "[gamepad]")
{
    constexpr float pi = 3.14159265f;
    // A full turn a second, the stick reported at one rate and read at another: at 144 frames
    // a second and 125 reports some frames see no new report, at 60 frames and 250 reports
    // each frame sees several.
    struct Rates {
        float frames;
        float reports;
    };
    for (Rates rates : { Rates { 144, 125 }, Rates { 60, 250 }, Rates { 240, 66 }, Rates { 165, 1000 } }) {
        dx::gamepad::FlickStick flick;
        const float frame = 1.0f / rates.frames;
        const float report = 1.0f / rates.reports;
        // Settled at the top of the rim.
        for (int i = 0; i < 20; i++) {
            flick.update(0.0f, -1.0f, frame);
        }
        int frames = static_cast<int>(rates.frames) * 2;
        float clock = 0;
        float perFrame = 2.0f * pi * frame;
        for (int i = 0; i < frames; i++) {
            clock += frame;
            float angle = 2.0f * pi * std::floor(clock / report) * report;
            float step = flick.update(std::sin(angle), -std::cos(angle), frame);
            // Once it has settled, after a second, no frame turns much more or less than a
            // frame's worth: within a tenth of it, or where frames see several reports each,
            // within the one report a frame's reading may fall short or past.
            if (i >= frames / 2) {
                INFO(rates.frames << " frames and " << rates.reports << " reports a second, frame " << i);
                CHECK(std::abs(step - perFrame) <= std::max(perFrame * 0.1f, 2.0f * pi * report * 1.01f));
            }
        }
    }
}

TEST_CASE("crossing the back of the rim turns the short way", "[gamepad]")
{
    constexpr float pi = 3.14159265f;
    dx::gamepad::FlickStick flick;
    // Just left of straight down, settled.
    for (int i = 0; i < 20; i++) {
        flick.update(-0.05f, 1.0f, 0.01f);
    }
    // Just right of it: a small turn, not nearly a whole one.
    float step = flick.update(0.05f, 1.0f, 0.01f);
    CHECK(std::abs(step) < pi * 0.1f);
}

TEST_CASE("the remapping layout reads the ini's button names", "[gamepad]")
{
    using namespace dx::gamepad;
    std::vector<std::string> values(ButtonCount);
    values[A] = "Y";
    values[Y] = "a";
    values[X] = "none";
    values[B] = "nonsense";
    values[RightPaddle1] = "lb";
    Layout layout = parseLayout(values);
    CHECK(layout[A] == Y);
    CHECK(layout[Y] == A);
    CHECK(layout[X] == ButtonCount);
    CHECK(layout[B] == B);
    CHECK(layout[RightPaddle1] == LeftShoulder);
    CHECK(layout[LeftPaddle1] == LeftPaddle1);
    CHECK(buttonName(RightPaddle2) == std::string("r5"));
    CHECK(buttonFromName("l5") == LeftPaddle2);
    CHECK(fromSdl(SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, false) == LeftPaddle1);
}

TEST_CASE("the back paddles hold gyro aiming off and send no key", "[gamepad]")
{
    using namespace dx::gamepad;
    for (Button paddle : { RightPaddle1, LeftPaddle1, RightPaddle2, LeftPaddle2 }) {
        CHECK(triggers(Action::GyroOff, paddle));
        CHECK(buttonKey(paddle) == dx::IK_None);
        CHECK(glyphName(paddle) != nullptr);
    }
    for (const Binding& binding : defaultBindings()) {
        CHECK(binding.key != dx::IK_None);
    }
}

TEST_CASE("play prompts show the input that plays the role, menus keep their own", "[gamepad]")
{
    using namespace dx::gamepad;
    std::vector<std::string> values(ButtonCount);
    values[A] = "y";
    values[Y] = "a";
    values[RightPaddle1] = "x";
    values[X] = "none";
    setLayout(parseLayout(values));
    // Jump is Y's role, now on A; Interact A's, now on Y.
    CHECK(shownControl(Action::Jump).button == A);
    CHECK(shownControl(Action::Interact).button == Y);
    // Reload is X's role, now only on a paddle.
    CHECK(playedBy(X) == RightPaddle1);
    CHECK(shownControl(Action::Reload).button == RightPaddle1);
    // Menus are not remapped.
    CHECK(shownControl(Action::MenuSelect).button == A);
    CHECK(role(A) == Y);
    setLayout(defaultLayout());
    CHECK(shownControl(Action::Jump).button == Y);
}

TEST_CASE("triggers press past half travel and release below a third", "[gamepad]")
{
    CHECK_FALSE(dx::gamepad::triggerHeld(0.4f, false));
    CHECK(dx::gamepad::triggerHeld(0.6f, false));
    CHECK(dx::gamepad::triggerHeld(0.4f, true));
    CHECK_FALSE(dx::gamepad::triggerHeld(0.2f, true));
}

TEST_CASE("digital triggers, which report only rest or full travel, press and release", "[gamepad]")
{
    CHECK(dx::gamepad::triggerHeld(1.0f, false));
    CHECK(dx::gamepad::triggerHeld(1.0f, true));
    CHECK_FALSE(dx::gamepad::triggerHeld(0.0f, true));
    CHECK_FALSE(dx::gamepad::triggerHeld(0.0f, false));
}

TEST_CASE("Nintendo's A and B put A's actions on the right face button and B's on the bottom one", "[gamepad]")
{
    using namespace dx::gamepad;
    CHECK(fromSdl(SDL_GAMEPAD_BUTTON_SOUTH, false) == A);
    CHECK(fromSdl(SDL_GAMEPAD_BUTTON_EAST, false) == B);
    CHECK(fromSdl(SDL_GAMEPAD_BUTTON_EAST, true) == A);
    CHECK(fromSdl(SDL_GAMEPAD_BUTTON_SOUTH, true) == B);
    // X and Y stay where they are.
    for (bool nintendo : { false, true }) {
        CHECK(fromSdl(SDL_GAMEPAD_BUTTON_WEST, nintendo) == X);
        CHECK(fromSdl(SDL_GAMEPAD_BUTTON_NORTH, nintendo) == Y);
        CHECK(placeOf(X, nintendo) == X);
        CHECK(placeOf(Y, nintendo) == Y);
    }
    // A prompt shows the place the player presses: the east button's glyph for A.
    CHECK(placeOf(A, true) == B);
    CHECK(placeOf(B, true) == A);
    CHECK(placeOf(A, false) == A);
}

TEST_CASE("gamepad buttons map to the joystick keys and the d-pad to the POV hat", "[gamepad]")
{
    CHECK(dx::gamepad::buttonKey(dx::gamepad::A) == dx::IK_Joy1);
    CHECK(dx::gamepad::buttonKey(dx::gamepad::RightTrigger) == dx::IK_Joy1 + 11);
    CHECK(dx::gamepad::buttonKey(dx::gamepad::DPadUp) == dx::IK_JoyPovUp);
    CHECK(dx::gamepad::buttonKey(dx::gamepad::DPadRight) == dx::IK_JoyPovRight);
}

TEST_CASE("every action is mapped to a button", "[gamepad]")
{
    for (int i = 0; i <= static_cast<int>(dx::gamepad::Action::Autologin); i++) {
        auto action = static_cast<dx::gamepad::Action>(i);
        INFO("action " << i);
        CHECK(dx::gamepad::mapping(action).action == action);
        CHECK(dx::gamepad::button(action) < dx::gamepad::ButtonCount);
    }
}

TEST_CASE("the default bindings come from the mapping", "[gamepad]")
{
    using namespace dx;
    std::vector<std::pair<uint8_t, std::string>> expected {
        { IK_JoyX, "Axis aStrafe Speed=+300.0" },
        { IK_JoyY, "Axis aBaseY Speed=-300.0" },
        { IK_JoyU, "Axis aBaseX Speed=+250.0" },
        { IK_JoyV, "Axis aLookUp Speed=-150.0" },
        { IK_JoyZ, "" },
        { IK_JoyR, "" },
        { IK_Joy1 + 0, "ParseRightClick" },
        { IK_Joy1 + 1, "Toggle bDuck" },
        { IK_Joy1 + 2, "ReloadWeapon" },
        { IK_Joy1 + 3, "Jump" },
        { IK_Joy1 + 4, "" },
        { IK_Joy1 + 5, "" },
        { IK_Joy1 + 6, "ShowInventoryWindow" },
        { IK_Joy1 + 7, "ShowMainMenu" },
        { IK_Joy1 + 8, "ToggleScope" },
        { IK_Joy1 + 9, "ToggleLaser" },
        { IK_Joy1 + 10, "" },
        { IK_Joy1 + 11, "ParseLeftClick|Fire" },
        { IK_JoyPovLeft, "" },
        { IK_JoyPovRight, "" },
        { IK_JoyPovUp, "" },
        { IK_JoyPovDown, "" },
        { gamepad::leanKey, "Axis aExtra0 Speed=+0.05" },
        { gamepad::walkKey, "Button bRun" },
    };
    std::vector<std::pair<uint8_t, std::string>> actual;
    for (const gamepad::Binding& binding : gamepad::defaultBindings()) {
        actual.emplace_back(binding.key, binding.command);
    }
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    CHECK(actual == expected);
}

TEST_CASE("every mapped control and button has a glyph in every controller style", "[gamepad][data]")
{
    dx::engine::initialize(dx::test::gameData());
    for (dx::glyphs::Style style : { dx::glyphs::Style::Xbox, dx::glyphs::Style::PlayStation, dx::glyphs::Style::Switch }) {
        dx::glyphs::setStyle(style);
        for (const dx::gamepad::Mapping& mapping : dx::gamepad::mappings()) {
            INFO("action " << static_cast<int>(mapping.action));
            CHECK(dx::glyphs::image(mapping.control) != nullptr);
        }
        for (int i = 0; i < dx::gamepad::ButtonCount; i++) {
            CHECK(dx::glyphs::image(static_cast<dx::gamepad::Button>(i)) != nullptr);
        }
    }
    dx::glyphs::setStyle(dx::glyphs::Style::Xbox);
}

TEST_CASE("stick buttons use click glyphs distinct from stick movement for every controller style", "[gamepad][data]")
{
    dx::engine::initialize(dx::test::gameData());
    for (dx::glyphs::Style style : { dx::glyphs::Style::Xbox, dx::glyphs::Style::PlayStation, dx::glyphs::Style::Switch }) {
        dx::glyphs::setStyle(style);
        REQUIRE(dx::glyphs::image("pad_ls_click") != nullptr);
        REQUIRE(dx::glyphs::image("pad_rs_click") != nullptr);
        CHECK(dx::glyphs::image(dx::gamepad::LeftStick) == dx::glyphs::image("pad_ls_click"));
        CHECK(dx::glyphs::image(dx::gamepad::RightStick) == dx::glyphs::image("pad_rs_click"));
        CHECK(dx::glyphs::image(dx::gamepad::LeftStick) != dx::glyphs::image("pad_ls"));
        CHECK(dx::glyphs::image(dx::gamepad::RightStick) != dx::glyphs::image("pad_rs"));
    }
    dx::glyphs::setStyle(dx::glyphs::Style::Xbox);
}

TEST_CASE("PlayStation inventory shows the touchpad click, not Create/Share", "[gamepad][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::glyphs::setStyle(dx::glyphs::Style::PlayStation);
    CHECK(dx::glyphs::image("pad_view") == dx::glyphs::image("ps_touchpad"));
    CHECK(dx::glyphs::image("pad_view") != dx::glyphs::image("ps_view"));
    dx::glyphs::setStyle(dx::glyphs::Style::Xbox);
}

TEST_CASE("in menus A activates, B backs out and the d-pad steps like arrow keys", "[gamepad]")
{
    CHECK(dx::gamepad::menuKey(dx::gamepad::A) == dx::IK_Enter);
    CHECK(dx::gamepad::menuKey(dx::gamepad::B) == dx::IK_Escape);
    CHECK(dx::gamepad::menuKey(dx::gamepad::View) == dx::IK_Escape);
    CHECK(dx::gamepad::menuKey(dx::gamepad::Menu) == dx::IK_Escape);
    CHECK(dx::gamepad::menuKey(dx::gamepad::DPadUp) == dx::IK_Up);
    CHECK(dx::gamepad::menuKey(dx::gamepad::X) == dx::IK_None);
    CHECK(dx::gamepad::menuKey(dx::gamepad::DPadLeft) == dx::IK_Left);
    CHECK(dx::gamepad::menuKey(dx::gamepad::RightTrigger) == dx::IK_None);
}

TEST_CASE("the gameplay diagram has distinct embedded art for each controller style", "[gamepad][data]")
{
    dx::engine::initialize(dx::test::gameData());
    std::vector<dx::UTexture*> images;
    for (dx::glyphs::Style style : { dx::glyphs::Style::Xbox, dx::glyphs::Style::PlayStation, dx::glyphs::Style::Switch }) {
        dx::glyphs::setStyle(style);
        dx::UTexture* image = dx::glyphs::image("pad_layout");
        REQUIRE(image != nullptr);
        CHECK(image->width() == 768);
        CHECK(image->height() == 768);
        CHECK(std::find(images.begin(), images.end(), image) == images.end());
        images.push_back(image);
    }
    dx::glyphs::setStyle(dx::glyphs::Style::Xbox);
}

TEST_CASE("Controls opens the read-only gameplay diagram and Back returns to Controls", "[gamepad][data]")
{
    class LayoutCanvas : public dx::Canvas2D {
    public:
        struct Segment {
            float x1, y1, x2, y2;
        };

        std::vector<Segment> pendingLines;
        std::vector<Segment> lookLines;
        std::vector<Segment> laserLines;
        std::vector<Segment> moveLines;
        std::vector<Segment> scopeLines;
        dx::UTexture* expected = nullptr;
        int illustrations = 0;
        int leaders = 0;
        int buttonGlyphs = 0;
        int lookGlyphs = 0;
        int laserGlyphs = 0;
        dx::UTexture* look = nullptr;
        dx::UTexture* laser = nullptr;
        dx::UTexture* move = nullptr;
        dx::UTexture* scope = nullptr;
        dx::LinearColor artColor;
        std::vector<dx::UTexture*> buttons;
        bool checkingLayout = false;
        float canvasWidth = 1280;
        float canvasHeight = 720;

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

        void drawTile(dx::UTexture* texture, float x, float y, float w, float h, float, float, float, float, dx::LinearColor color, dx::BlendStyle, bool) override
        {
            if (expected && texture == expected) {
                illustrations++;
                CHECK(x >= 0);
                CHECK(y >= 0);
                CHECK(x + w <= width());
                CHECK(y + h <= height());
                artColor = color;
            }
            if (checkingLayout && std::find(buttons.begin(), buttons.end(), texture) != buttons.end()) {
                buttonGlyphs++;
                lookGlyphs += texture == look ? 1 : 0;
                laserGlyphs += texture == laser ? 1 : 0;
                CHECK(color.r == artColor.r);
                CHECK(color.g == artColor.g);
                CHECK(color.b == artColor.b);
                if (texture == look) {
                    lookLines = pendingLines;
                } else if (texture == laser) {
                    laserLines = pendingLines;
                } else if (texture == move) {
                    moveLines = pendingLines;
                } else if (texture == scope) {
                    scopeLines = pendingLines;
                }
                pendingLines.clear();
            }
        }

        void drawRect(float, float, float, float, dx::LinearColor, dx::BlendStyle) override
        {
        }

        void drawLine(float x1, float y1, float x2, float y2, dx::LinearColor) override
        {
            if (checkingLayout) {
                leaders++;
                pendingLines.push_back({ x1, y1, x2, y2 });
                CHECK(x1 >= 0);
                CHECK(y1 >= 0);
                CHECK(x2 >= 0);
                CHECK(y2 >= 0);
                CHECK(x1 <= width());
                CHECK(y1 <= height());
                CHECK(x2 <= width());
                CHECK(y2 <= height());
            }
        }
    };

    dx::engine::initialize(dx::test::gameData());
    dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
    dx::padlayout::install();
    auto size = GENERATE(std::pair { 640.0f, 480.0f }, std::pair { 1280.0f, 720.0f }, std::pair { 1920.0f, 1080.0f });
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* root = dx::extension::rootWindow();
        REQUIRE(root != nullptr);
        LayoutCanvas canvas;
        canvas.canvasWidth = size.first;
        canvas.canvasHeight = size.second;
        dx::extension::render(canvas);
        dx::UObject* button = dx::test::openLayoutRow();
        REQUIRE(button != nullptr);
        dx::UObject* controls = dx::extension::topModal();
        CHECK_FALSE(dx::padlayout::buttonPressed(dx::script::object(controls, "winClient")));
        for (dx::glyphs::Style style : { dx::glyphs::Style::Xbox, dx::glyphs::Style::PlayStation, dx::glyphs::Style::Switch }) {
            dx::glyphs::setStyle(style);
            canvas.illustrations = 0;
            canvas.leaders = 0;
            canvas.buttonGlyphs = 0;
            canvas.lookGlyphs = 0;
            canvas.laserGlyphs = 0;
            canvas.pendingLines.clear();
            canvas.lookLines.clear();
            canvas.laserLines.clear();
            canvas.moveLines.clear();
            canvas.scopeLines.clear();
            canvas.buttons.clear();
            for (const char* name : { "pad_view", "pad_lt", "pad_lb", "pad_ls", "pad_ls_click", "pad_dpad", "pad_menu", "pad_rt", "pad_rb", "pad_y", "pad_b", "pad_a", "pad_x", "pad_rs", "pad_rs_click" }) {
                canvas.buttons.push_back(dx::glyphs::image(name));
            }
            canvas.look = dx::glyphs::image("pad_rs");
            canvas.laser = dx::glyphs::image("pad_rs_click");
            canvas.move = dx::glyphs::image("pad_ls");
            canvas.scope = dx::glyphs::image("pad_ls_click");
            canvas.checkingLayout = false;
            canvas.expected = dx::glyphs::image("pad_layout");
            REQUIRE(dx::padlayout::buttonPressed(button));
            dx::UObject* layout = dx::extension::topModal();
            REQUIRE(dx::script::isA(layout, "MenuScreenControllerLayout"));
            CHECK(dx::script::integer(layout, "choiceCount") == 0);
            CHECK_FALSE(dx::script::flag(layout, "bUsesHelpWindow"));
            CHECK(dx::script::integer(layout, "ClientWidth") >= 580);
            CHECK(dx::script::integer(layout, "ClientWidth") <= 1000);
            CHECK(static_cast<float>(dx::script::integer(layout, "ClientHeight")) <= size.second);
            dx::extension::render(canvas);
            // The diagram can preview a different style without changing the global setting.
            dx::glyphs::setStyle(dx::glyphs::Style::Xbox);
            canvas.checkingLayout = true;
            REQUIRE(dx::padlayout::draw(canvas));
            CHECK(canvas.illustrations == 1);
            CHECK(canvas.leaders >= 30);
            CHECK(canvas.buttonGlyphs == 15);
            CHECK(canvas.lookGlyphs == 1);
            CHECK(canvas.laserGlyphs == 1);
            // Each stick gets one shared stem into a bracket beside the guide, which then
            // branches horizontally to the movement and click icons. The click row must not
            // redraw the stem or introduce a second controller anchor.
            auto checkFork = [](const std::vector<LayoutCanvas::Segment>& movement, const std::vector<LayoutCanvas::Segment>& click) {
                REQUIRE(movement.size() == 4);
                REQUIRE(click.size() == 1);
                // The shared stem joins the middle of the bracket, whose vertical spans both rows.
                CHECK(movement[1].x2 == movement[2].x1);
                CHECK(movement[2].x1 == movement[2].x2);
                CHECK(movement[2].y1 != movement[2].y2);
                // Screen coordinates are rounded, so allow the stem join half a pixel either side.
                CHECK(std::abs(movement[1].y2 - (movement[2].y1 + movement[2].y2) * 0.5f) <= 1.0f);
                // Movement and click leave the bracket as horizontal branches on their own row.
                CHECK(movement[3].y1 == movement[3].y2);
                CHECK(click[0].y1 == click[0].y2);
                CHECK(movement[3].x1 == movement[2].x1);
                CHECK(click[0].x1 == movement[2].x1);
                CHECK(movement[3].y1 == movement[2].y1);
                CHECK(click[0].y1 == movement[2].y2);
            };
            checkFork(canvas.moveLines, canvas.scopeLines);
            checkFork(canvas.lookLines, canvas.laserLines);
            canvas.checkingLayout = false;
            CHECK(dx::glyphs::style() == dx::glyphs::Style::Xbox);
            dx::extension::keyEvent(dx::gamepad::menuKey(dx::gamepad::B), dx::IST_Press, 0);
            dx::extension::keyEvent(dx::IK_Escape, dx::IST_Release, 0);
            CHECK(dx::extension::topModal() == controls);
            CHECK_FALSE(dx::padlayout::draw(canvas));
        }
    }
}

TEST_CASE("the joystick key codes match the names in the engine's input enum", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    CHECK(dx::console::keyName(dx::IK_Joy1) == "Joy1");
    CHECK(dx::console::keyName(dx::IK_JoyX) == "JoyX");
    CHECK(dx::console::keyName(dx::IK_JoyR) == "JoyR");
    CHECK(dx::console::keyName(dx::IK_JoyU) == "JoyU");
    CHECK(dx::console::keyName(dx::IK_JoyV) == "JoyV");
    CHECK(dx::console::keyName(dx::IK_JoyPovUp) == "JoyPovUp");
    CHECK(dx::console::keyName(dx::IK_JoyPovRight) == "JoyPovRight");
}

TEST_CASE("the Xbox layout replaces the stock joystick bindings and is saved to the user ini", "[data]")
{
    // Only while there is no User.ini of Ambrosia's own yet.
    std::error_code ignored;
    std::filesystem::remove(dx::config::userDirectory() / "User.ini", ignored);
    dx::engine::initialize(dx::test::gameData());
    dx::console::loadBindings();
    CHECK(dx::console::binding(dx::IK_JoyX) == "Axis aStrafe Speed=+300.0");
    CHECK(dx::console::binding(dx::gamepad::buttonKey(dx::gamepad::A)) == "ParseRightClick");
    CHECK(dx::console::binding(dx::gamepad::buttonKey(dx::gamepad::X)) == "ReloadWeapon");
    CHECK(dx::console::binding(dx::gamepad::buttonKey(dx::gamepad::Y)) == "Jump");
    CHECK(dx::console::binding(dx::IK_Joy1 + 11) == "ParseLeftClick|Fire");
    CHECK(dx::config::user().get("Engine.Input", "Joy4") == "Jump");
    dx::Ini saved;
    REQUIRE(saved.load(dx::config::user().path));
    CHECK(saved.get("Engine.Input", "Joy4") == "Jump");
}

TEST_CASE("a rebound gamepad button survives reloading the bindings", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::console::loadBindings();
    std::string original = dx::console::binding(dx::IK_Joy1);
    dx::console::execute(nullptr, "SET InputExt Joy1 ToggleCrosshair");
    dx::console::loadBindings();
    CHECK(dx::console::binding(dx::IK_Joy1) == "ToggleCrosshair");
    dx::console::execute(nullptr, "SET InputExt Joy1 " + original);
}

namespace {
    std::string playerProperty(dx::UObject* player, const char* name)
    {
        dx::UProperty* property = player->cls->findProperty(dx::Name(name));
        return property ? property->toText(player->data + property->offset) : std::string();
    }
}

TEST_CASE("the lean axis leans a player standing still", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::console::loadBindings();
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        world.inputHook = [](dx::UObject* player, float delta) {
            dx::console::readInput(player, delta);
        };
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        for (int i = 0; i < 10; i++) {
            world.tick(1.0f / 30.0f);
        }
        dx::console::analog(dx::gamepad::leanKey, 1.0f);
        for (int i = 0; i < 30; i++) {
            world.tick(1.0f / 30.0f);
        }
        dx::console::analog(dx::gamepad::leanKey, 0.0f);
        CHECK(std::stof(playerProperty(player, "curLeanDist")) > 10.0f);
        dx::console::resetInput(player);
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("B toggles crouching on and off", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::console::loadBindings();
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        world.inputHook = [](dx::UObject* player, float delta) {
            dx::console::readInput(player, delta);
        };
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        auto tap = [&] {
            dx::console::key(dx::gamepad::buttonKey(dx::gamepad::B), dx::IST_Press, 0);
            world.tick(1.0f / 30.0f);
            dx::console::key(dx::gamepad::buttonKey(dx::gamepad::B), dx::IST_Release, 0);
            for (int i = 0; i < 15; i++) {
                world.tick(1.0f / 30.0f);
            }
        };
        tap();
        CHECK(playerProperty(player, "bIsCrouching") == "True");
        tap();
        CHECK(playerProperty(player, "bIsCrouching") == "False");
        dx::console::resetInput(player);
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("a fully pushed stick moves the player as fast as holding the forward key", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        uint8_t forward = dx::console::keyFromName("W");
        REQUIRE(dx::console::binding(forward) == "MoveForward");

        dx::console::resetInput(player);
        dx::console::key(forward, dx::IST_Press, 0);
        dx::console::readInput(player, 0.1f);
        float keyboard = dx::playerpawn::aBaseY(player);
        dx::console::key(forward, dx::IST_Release, 0);

        dx::console::resetInput(player);
        dx::console::analog(dx::IK_JoyY, -1.0f);
        dx::console::readInput(player, 0.1f);
        float stick = dx::playerpawn::aBaseY(player);

        dx::console::resetInput(player);
        dx::console::analog(dx::IK_JoyY, -0.5f);
        dx::console::readInput(player, 0.1f);
        float half = dx::playerpawn::aBaseY(player);
        dx::console::resetInput(player);

        CHECK(keyboard > 0.0f);
        CHECK(stick == Catch::Approx(keyboard));
        CHECK(half == Catch::Approx(keyboard * 0.5f));
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("A and LB act immediately while X reloads, puts away or switches ammo", "[gamepad][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        for (int i = 0; i < 10; i++) {
            world.tick(1.0f / 30.0f);
        }
        REQUIRE(dx::hud::shown());
        REQUIRE_FALSE(dx::extension::menuOpen());
        dx::console::execute(player, "giveitem DeusEx.Medkit");
        dx::UObject* item = nullptr;
        for (dx::UObject* entry : dx::script::inventory(player)) {
            if (dx::script::isA(entry, "MedKit")) {
                item = entry;
            }
        }
        REQUIRE(item != nullptr);
        dx::vm::Call(player, dx::Name("PutInHand")).obj(item).run();
        auto settle = [&] {
            for (int i = 0; i < 15; i++) {
                world.tick(1.0f / 30.0f);
            }
        };
        settle();
        REQUIRE(dx::script::object(player, "inHand") == item);

        REQUIRE(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
        SDL_VirtualJoystickDesc description {};
        description.version = sizeof(description);
        description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        description.naxes = SDL_GAMEPAD_AXIS_COUNT;
        description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        SDL_JoystickID id = SDL_AttachVirtualJoystick(&description);
        REQUIRE(id != 0);
        {
            std::vector<std::pair<uint8_t, bool>> keys;
            dx::game::GamepadInput pad({ .player = [&] { return player; }, .sendKey = [&](uint8_t key, bool down) { keys.emplace_back(key, down); }, .editText = nullptr });
            SDL_Event event {};
            event.type = SDL_EVENT_GAMEPAD_ADDED;
            event.gdevice.which = id;
            pad.handleEvent(event);
            auto button = [&](SDL_GamepadButton which, bool down) {
                event = {};
                event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
                event.gbutton.which = id;
                event.gbutton.button = static_cast<uint8_t>(which);
                pad.handleEvent(event);
            };

            SECTION("A sends its binding before release and never puts the item away")
            {
                button(SDL_GAMEPAD_BUTTON_SOUTH, true);
                REQUIRE(keys.size() == 1);
                CHECK(keys.front() == std::make_pair(dx::gamepad::buttonKey(dx::gamepad::A), true));
                pad.update(&world, 0.5f);
                button(SDL_GAMEPAD_BUTTON_SOUTH, false);
                REQUIRE(keys.size() == 2);
                CHECK(keys.back() == std::make_pair(dx::gamepad::buttonKey(dx::gamepad::A), false));
                CHECK(dx::script::object(player, "inHand") == item);
            }
            SECTION("a short X press reloads without putting away")
            {
                button(SDL_GAMEPAD_BUTTON_WEST, true);
                pad.update(&world, 0.1f);
                CHECK(pad.timeScale() == 1.0f);
                CHECK(dx::script::object(player, "inHand") == item);
                CHECK(keys.empty());
                button(SDL_GAMEPAD_BUTTON_WEST, false);
                REQUIRE(keys.size() == 2);
                CHECK(keys.front() == std::make_pair(dx::gamepad::buttonKey(dx::gamepad::X), true));
                CHECK(keys.back() == std::make_pair(dx::gamepad::buttonKey(dx::gamepad::X), false));
                settle();
                CHECK(dx::script::object(player, "inHand") == item);
            }
            SECTION("both radials ease into a full pause and smoothly resume")
            {
                for (SDL_GamepadButton bumper : { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER }) {
                    button(bumper, true);
                    CHECK(pad.timeScale() == 1.0f);
                    pad.update(&world, 0.1f);
                    CHECK(pad.timeScale() == Catch::Approx(0.5f));
                    pad.update(&world, 0.1f);
                    CHECK(pad.timeScale() == 0.0f);
                    pad.update(&world, 0.5f);
                    CHECK(pad.timeScale() == 0.0f);
                    button(bumper, false);
                    CHECK(pad.timeScale() == 0.0f);
                    pad.update(&world, 0.1f);
                    CHECK(pad.timeScale() == Catch::Approx(0.5f));
                    pad.update(&world, 0.1f);
                    CHECK(pad.timeScale() == 1.0f);
                    CHECK(dx::script::object(player, "inHand") == item);
                }
            }
            auto hold = [&](float seconds) {
                for (; seconds > 0; seconds -= 0.1f) {
                    pad.update(&world, 0.1f);
                }
            };
            SECTION("holding X puts away once without reloading")
            {
                button(SDL_GAMEPAD_BUTTON_WEST, true);
                hold(0.4f);
                settle();
                CHECK(dx::script::object(player, "inHand") == nullptr);
                hold(0.4f);
                button(SDL_GAMEPAD_BUTTON_WEST, false);
                CHECK(keys.empty());
                // Tapping with empty hands restores that exact item, without a reload.
                button(SDL_GAMEPAD_BUTTON_WEST, true);
                button(SDL_GAMEPAD_BUTTON_WEST, false);
                CHECK(dx::script::object(player, "inHandPending") == item);
                settle();
                CHECK(dx::script::object(player, "inHand") == item);
                CHECK(keys.empty());
            }
            SECTION("closing a radial mid-ramp reverses smoothly")
            {
                button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
                pad.update(&world, 0.05f);
                float slowed = pad.timeScale();
                CHECK(slowed > 0.0f);
                CHECK(slowed < 1.0f);
                button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
                CHECK(pad.timeScale() == slowed);
                pad.update(&world, 0.025f);
                CHECK(pad.timeScale() > slowed);
                CHECK(pad.timeScale() < 1.0f);
                pad.update(&world, 0.025f);
                CHECK(pad.timeScale() == 1.0f);
            }
            SECTION("X does not redraw an item that has left the inventory")
            {
                button(SDL_GAMEPAD_BUTTON_WEST, true);
                hold(0.4f);
                button(SDL_GAMEPAD_BUTTON_WEST, false);
                settle();
                REQUIRE(dx::script::object(player, "inHand") == nullptr);
                dx::vm::Call(player, dx::Name("DeleteInventory")).obj(item).run();
                button(SDL_GAMEPAD_BUTTON_WEST, true);
                button(SDL_GAMEPAD_BUTTON_WEST, false);
                settle();
                CHECK(dx::script::object(player, "inHandPending") == nullptr);
                CHECK(dx::script::object(player, "inHand") == nullptr);
            }
            SECTION("LB plus X switches ammo without a selected radial slice")
            {
                dx::console::execute(player, "giveitem DeusEx.WeaponSawedOffShotgun");
                dx::console::execute(player, "giveitem DeusEx.AmmoShell");
                dx::console::execute(player, "giveitem DeusEx.AmmoSabot");
                dx::UObject* weapon = nullptr;
                for (dx::UObject* entry : dx::script::inventory(player)) {
                    if (dx::script::isA(entry, "WeaponSawedOffShotgun")) {
                        weapon = entry;
                    }
                }
                REQUIRE(weapon != nullptr);
                dx::vm::Call(player, dx::Name("PutInHand")).obj(weapon).run();
                settle();
                REQUIRE(dx::script::object(player, "inHand") == weapon);
                dx::UObject* ammo = dx::script::object(weapon, "AmmoName");
                button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
                pad.update(&world, 0.1f);
                button(SDL_GAMEPAD_BUTTON_WEST, true);
                CHECK(dx::script::object(weapon, "AmmoName") != ammo);
                CHECK(pad.timeScale() < 1.0f);
                // Release LB first: X must not turn into a reload or holster afterward.
                button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
                pad.update(&world, 0.5f);
                button(SDL_GAMEPAD_BUTTON_WEST, false);
                settle();
                CHECK(dx::script::object(player, "inHand") == weapon);
                CHECK(std::none_of(keys.begin(), keys.end(), [](const auto& key) { return key.first == dx::gamepad::buttonKey(dx::gamepad::X) && key.second; }));
            }
            SECTION("a menu opening while X is held does not put away")
            {
                button(SDL_GAMEPAD_BUTTON_WEST, true);
                dx::vm::Call(player, dx::Name("ShowInventoryWindow")).run();
                REQUIRE(dx::extension::menuOpen());
                pad.update(&world, 0.5f);
                button(SDL_GAMEPAD_BUTTON_WEST, false);
                CHECK(dx::script::object(player, "inHand") == item);
            }
            SECTION("a stalled frame does not turn an X tap into a put away")
            {
                button(SDL_GAMEPAD_BUTTON_WEST, true);
                pad.update(&world, 2.0f);
                button(SDL_GAMEPAD_BUTTON_WEST, false);
                REQUIRE(keys.size() == 2);
                CHECK(keys.front() == std::make_pair(dx::gamepad::buttonKey(dx::gamepad::X), true));
                settle();
                CHECK(dx::script::object(player, "inHand") == item);
            }
            SECTION("a button let go while a radial is open releases its key")
            {
                uint8_t jump = dx::gamepad::buttonKey(dx::gamepad::A);
                button(SDL_GAMEPAD_BUTTON_SOUTH, true);
                button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
                button(SDL_GAMEPAD_BUTTON_SOUTH, false);
                REQUIRE(keys.size() == 2);
                CHECK(keys.back() == std::make_pair(jump, false));
                button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
                button(SDL_GAMEPAD_BUTTON_SOUTH, true);
                REQUIRE(keys.size() == 3);
                CHECK(keys.back() == std::make_pair(jump, true));
                button(SDL_GAMEPAD_BUTTON_SOUTH, false);
            }
            SECTION("unplugging with a radial open releases the held keys")
            {
                uint8_t jump = dx::gamepad::buttonKey(dx::gamepad::A);
                button(SDL_GAMEPAD_BUTTON_SOUTH, true);
                button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
                pad.unplugged();
                CHECK(std::count(keys.begin(), keys.end(), std::make_pair(jump, false)) == 1);
                CHECK(std::none_of(keys.begin(), keys.end(), [](const auto& key) { return key.first == dx::IK_None; }));
                dx::vm::Call(dx::extension::rootWindow(), dx::Name("PopWindow")).run();
            }
            SECTION("another pad takes over a radial and an unplug without the main menu")
            {
                SDL_JoystickID other = attachPad();
                REQUIRE(other != 0);
                padEvent(pad, SDL_EVENT_GAMEPAD_ADDED, other);
                uint8_t jump = dx::gamepad::buttonKey(dx::gamepad::A);
                button(SDL_GAMEPAD_BUTTON_SOUTH, true);
                button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
                pad.update(&world, 0.1f);
                REQUIRE(pad.timeScale() < 1.0f);
                // The other pad's first press closes the wheel and lets go of the held jump.
                padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_DOWN, other, SDL_GAMEPAD_BUTTON_DPAD_UP);
                CHECK(pad.activeController() == other);
                CHECK(std::count(keys.begin(), keys.end(), std::make_pair(jump, false)) == 1);
                pad.update(&world, 0.2f);
                CHECK(pad.timeScale() == 1.0f);
                padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_UP, other, SDL_GAMEPAD_BUTTON_DPAD_UP);
                // Unplugging the pad in play falls back to the first one and stays in play.
                padEvent(pad, SDL_EVENT_GAMEPAD_REMOVED, other);
                CHECK(pad.activeController() == id);
                CHECK_FALSE(dx::extension::menuOpen());
                padEvent(pad, SDL_EVENT_GAMEPAD_REMOVED, id);
                CHECK(pad.activeController() == 0);
                CHECK(dx::extension::menuOpen());
                dx::vm::Call(dx::extension::rootWindow(), dx::Name("PopWindow")).run();
                SDL_DetachVirtualJoystick(other);
            }
            SECTION("a d-pad held into play stops repeating in later menus")
            {
                dx::console::execute(player, "ShowMainMenu");
                REQUIRE(dx::extension::menuOpen());
                button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
                dx::vm::Call(dx::extension::rootWindow(), dx::Name("PopWindow")).run();
                REQUIRE_FALSE(dx::extension::menuOpen());
                button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
                dx::console::execute(player, "ShowMainMenu");
                REQUIRE(dx::extension::menuOpen());
                dx::UObject* focus = dx::extension::focusWindow();
                hold(1.0f);
                CHECK(dx::extension::focusWindow() == focus);
                dx::vm::Call(dx::extension::rootWindow(), dx::Name("PopWindow")).run();
            }
            SECTION("crouch held into a radial stands up when let go there")
            {
                dx::UProperty* toggle = player->cls->findProperty(dx::Name("bToggleCrouch"));
                REQUIRE(toggle != nullptr);
                toggle->fromText(player->data + toggle->offset, "True");
                button(SDL_GAMEPAD_BUTTON_EAST, true);
                CHECK(dx::pawn::bDuck(player) == 1);
                button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
                REQUIRE(pad.timeScale() == 1.0f);
                pad.update(&world, 0.1f);
                REQUIRE(pad.timeScale() < 1.0f);
                button(SDL_GAMEPAD_BUTTON_EAST, false);
                CHECK(dx::pawn::bDuck(player) == 0);
                button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
                toggle->fromText(player->data + toggle->offset, "False");
            }
            SECTION("a tap begun before a radial opened does not go off after it closes")
            {
                uint8_t view = dx::gamepad::buttonKey(dx::gamepad::View);
                button(SDL_GAMEPAD_BUTTON_BACK, true);
                button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
                pad.update(&world, 0.1f);
                button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
                button(SDL_GAMEPAD_BUTTON_BACK, false);
                CHECK(std::none_of(keys.begin(), keys.end(), [view](const auto& key) { return key.first == view; }));
                CHECK_FALSE(dx::extension::menuOpen());
            }
        }
        SDL_DetachVirtualJoystick(id);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        dx::console::resetInput(player);
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("the last pad used is the one in play and the other lets go of its keys", "[gamepad]")
{
    REQUIRE(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
    SDL_JoystickID first = attachPad();
    SDL_JoystickID second = attachPad();
    REQUIRE(first != 0);
    REQUIRE(second != 0);
    // Earlier tests can leave a menu up, which would turn the buttons into menu keys.
    dx::extension::reset();
    REQUIRE_FALSE(dx::extension::menuOpen());
    {
        std::vector<std::pair<uint8_t, bool>> keys;
        dx::game::GamepadInput pad({ .player = [] { return nullptr; }, .sendKey = [&](uint8_t key, bool down) { keys.emplace_back(key, down); }, .editText = nullptr });
        padEvent(pad, SDL_EVENT_GAMEPAD_ADDED, first);
        padEvent(pad, SDL_EVENT_GAMEPAD_ADDED, second);
        CHECK(pad.activeController() == first);
        uint8_t jump = dx::gamepad::buttonKey(dx::gamepad::A);
        uint8_t y = dx::gamepad::buttonKey(dx::gamepad::Y);
        padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_DOWN, first, SDL_GAMEPAD_BUTTON_SOUTH);
        REQUIRE(keys == std::vector<std::pair<uint8_t, bool>> { { jump, true } });

        SECTION("a button press switches pads and releases what the old one held")
        {
            padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_DOWN, second, SDL_GAMEPAD_BUTTON_NORTH);
            CHECK(pad.activeController() == second);
            CHECK(keys == std::vector<std::pair<uint8_t, bool>> { { jump, true }, { jump, false }, { y, true } });
            // The old pad's buttons no longer reach the game.
            padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_UP, first, SDL_GAMEPAD_BUTTON_SOUTH);
            padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_DOWN, second, SDL_GAMEPAD_BUTTON_NORTH);
            CHECK(keys.size() == 3);
            padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_UP, second, SDL_GAMEPAD_BUTTON_NORTH);
            CHECK(keys.back() == std::make_pair(y, false));
            // Holding the same button on both pads across a switch back still pairs up.
            padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_DOWN, second, SDL_GAMEPAD_BUTTON_SOUTH);
            padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_DOWN, first, SDL_GAMEPAD_BUTTON_SOUTH);
            CHECK(pad.activeController() == first);
            padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_UP, first, SDL_GAMEPAD_BUTTON_SOUTH);
            CHECK(std::count(keys.begin(), keys.end(), std::make_pair(jump, true)) == std::count(keys.begin(), keys.end(), std::make_pair(jump, false)));
        }
        SECTION("an axis pushed past the used threshold switches pads")
        {
            padEvent(pad, SDL_EVENT_GAMEPAD_AXIS_MOTION, second, SDL_GAMEPAD_AXIS_LEFTX, 8000);
            CHECK(pad.activeController() == first);
            CHECK(keys.size() == 1);
            padEvent(pad, SDL_EVENT_GAMEPAD_AXIS_MOTION, second, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 30000);
            CHECK(pad.activeController() == second);
            CHECK(keys.back() == std::make_pair(jump, false));
            padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_UP, first, SDL_GAMEPAD_BUTTON_SOUTH);
            CHECK(keys.size() == 2);
        }
        SECTION("removing the pad in play hands over to the other one")
        {
            padEvent(pad, SDL_EVENT_GAMEPAD_REMOVED, first);
            CHECK(pad.activeController() == second);
            CHECK(keys.back() == std::make_pair(jump, false));
            padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_DOWN, second, SDL_GAMEPAD_BUTTON_NORTH);
            CHECK(keys.back() == std::make_pair(y, true));
            CHECK(keys.size() == 3);
            padEvent(pad, SDL_EVENT_GAMEPAD_REMOVED, second);
            CHECK(pad.activeController() == 0);
            CHECK(keys.back() == std::make_pair(y, false));
        }
        SECTION("removing the other pad leaves the one in play alone")
        {
            padEvent(pad, SDL_EVENT_GAMEPAD_REMOVED, second);
            CHECK(pad.activeController() == first);
            CHECK(keys.size() == 1);
            padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_UP, first, SDL_GAMEPAD_BUTTON_SOUTH);
            CHECK(keys.back() == std::make_pair(jump, false));
        }
    }
    SDL_DetachVirtualJoystick(second);
    SDL_DetachVirtualJoystick(first);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

TEST_CASE("the pad used last picks the prompts the Auto setting shows", "[gamepad]")
{
    REQUIRE(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
    // Virtual pads with the vendor and product of a DualSense and a Switch Pro Controller,
    // which SDL takes for those pads.
    SDL_JoystickID xbox = attachPad();
    SDL_JoystickID sony = attachPad(0x054c, 0x0ce6);
    SDL_JoystickID nintendo = attachPad(0x057e, 0x2009);
    REQUIRE(xbox != 0);
    REQUIRE(sony != 0);
    REQUIRE(nintendo != 0);
    CHECK(SDL_GetGamepadTypeForID(sony) == SDL_GAMEPAD_TYPE_PS5);
    CHECK(SDL_GetGamepadTypeForID(nintendo) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO);
    dx::extension::reset();
    dx::glyphs::Style before = dx::glyphs::padStyle();
    {
        dx::game::GamepadInput pad({ .player = [] { return nullptr; }, .sendKey = [](uint8_t, bool) {}, .editText = nullptr });
        padEvent(pad, SDL_EVENT_GAMEPAD_ADDED, xbox);
        CHECK(dx::glyphs::padStyle() == dx::glyphs::Style::Xbox);
        // A pad just connected is taken to be the one picked up.
        padEvent(pad, SDL_EVENT_GAMEPAD_ADDED, sony);
        CHECK(dx::glyphs::padStyle() == dx::glyphs::Style::PlayStation);
        padEvent(pad, SDL_EVENT_GAMEPAD_ADDED, nintendo);
        CHECK(dx::glyphs::padStyle() == dx::glyphs::Style::Switch);
        // A press, or a push past the used threshold, moves them; a light push does not.
        padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_DOWN, xbox, SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(dx::glyphs::padStyle() == dx::glyphs::Style::Xbox);
        padEvent(pad, SDL_EVENT_GAMEPAD_AXIS_MOTION, sony, SDL_GAMEPAD_AXIS_LEFTX, 8000);
        CHECK(dx::glyphs::padStyle() == dx::glyphs::Style::Xbox);
        padEvent(pad, SDL_EVENT_GAMEPAD_AXIS_MOTION, sony, SDL_GAMEPAD_AXIS_LEFTX, -30000);
        CHECK(dx::glyphs::padStyle() == dx::glyphs::Style::PlayStation);
        // A release is not a use.
        padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_UP, xbox, SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(dx::glyphs::padStyle() == dx::glyphs::Style::PlayStation);
        // Unplugging the pad in use goes back to the one used before it.
        padEvent(pad, SDL_EVENT_GAMEPAD_REMOVED, sony);
        CHECK(dx::glyphs::padStyle() == dx::glyphs::Style::Xbox);
        padEvent(pad, SDL_EVENT_GAMEPAD_BUTTON_DOWN, nintendo, SDL_GAMEPAD_BUTTON_EAST);
        CHECK(dx::glyphs::padStyle() == dx::glyphs::Style::Switch);
    }
    dx::glyphs::setPadStyle(before);
    SDL_DetachVirtualJoystick(nintendo);
    SDL_DetachVirtualJoystick(sony);
    SDL_DetachVirtualJoystick(xbox);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
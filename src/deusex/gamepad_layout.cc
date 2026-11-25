#include "deusex/gamepad_layout.h"

#include "core/objects.h"
#include "core/vm.h"
#include "deusex/deusex.h"
#include "deusex/glyphs.h"
#include "deusex/hud.h"
#include "deusex/menu_backdrop.h"
#include "deusex/menu_layout.h"
#include "deusex/references.h"
#include "deusex/script.h"
#include "deusex/ui.h"
#include "engine/resources.h"
#include "extension/extension.h"
#include "extension/windows.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace dx::padlayout {
    namespace {
        constexpr const char* screenName = "MenuScreenControllerLayout";
        constexpr float diagramWidth = 620;
        constexpr float diagramHeight = 360;
        glyphs::Style previewStyle = glyphs::Style::Xbox;
        // The Button Prompts choice on the Controls screen when the layout was opened, or the
        // style then shown where there is no choice. Auto (any value past the styles) is read
        // again each frame, so the diagram follows a change of pad while it is up.
        int previewChoice = 0;
        // The pad in play has back paddles (showPaddles).
        bool paddlesShown = false;

        // The screen class install makes. Script names it nowhere, so without this the first
        // level change would delete it.
        deusex::HeldObject<UClass>& layoutScreen()
        {
            static deusex::HeldObject<UClass> held;
            return held;
        }

        // Shows the prompt style chosen on the Controls screen while the layout is up, without
        // applying or saving that screen, so its Cancel still discards the player's changes.
        struct Preview {
            glyphs::Style previous = glyphs::style();

            Preview()
            {
                glyphs::setStyle(previewStyle);
            }

            ~Preview()
            {
                glyphs::setStyle(previous);
            }
        };

        // A member of one of a screen's action buttons, or null when the screen has none.
        uint8_t* actionField(UObject* screen, int index, const char* member)
        {
            UProperty* buttons = screen && screen->cls ? screen->cls->findProperty(Name("actionButtons")) : nullptr;
            UProperty* part = buttons && buttons->structType ? buttons->structType->findProperty(Name(member)) : nullptr;
            uint8_t* entry = part ? screen->propertyAddress(Name("actionButtons"), index) : nullptr;
            return entry ? entry + part->offset : nullptr;
        }

        // The same, recording what is missing for a startup patch.
        uint8_t* actionField(deusex::MenuPatch& patch, UObject* screen, int index, const char* member)
        {
            uint8_t* address = actionField(screen, index, member);
            if (!address) {
                patch.miss("actionButtons[" + std::to_string(index) + "]." + member);
            }
            return address;
        }

        struct Point {
            float x;
            float y;
        };

        // Button centers in the cropped, square controller textures, in Button enum order.
        // These belong to the artwork, not the bindings: the Switch face labels are positional.
        constexpr std::array<std::array<Point, gamepad::ButtonCount>, 3> anchors { {
            { {
                { 0.748f, 0.412f },
                { 0.812f, 0.348f },
                { 0.684f, 0.348f },
                { 0.748f, 0.284f },
                { 0.236f, 0.207f },
                { 0.764f, 0.207f },
                { 0.428f, 0.348f },
                { 0.569f, 0.348f },
                { 0.248f, 0.353f },
                { 0.628f, 0.496f },
                { 0.236f, 0.168f },
                { 0.764f, 0.168f },
                { 0.369f, 0.473f },
                { 0.369f, 0.540f },
                { 0.336f, 0.507f },
                { 0.402f, 0.507f },
            } },
            { {
                { 0.810f, 0.437f },
                { 0.885f, 0.364f },
                { 0.737f, 0.364f },
                { 0.810f, 0.291f },
                { 0.204f, 0.211f },
                { 0.796f, 0.211f },
                // The touchpad click, not Create/Share, opens the inventory on a PS5 pad.
                { 0.500f, 0.260f },
                { 0.732f, 0.254f },
                { 0.338f, 0.500f },
                { 0.662f, 0.500f },
                { 0.192f, 0.184f },
                { 0.808f, 0.184f },
                { 0.188f, 0.316f },
                { 0.188f, 0.412f },
                { 0.140f, 0.364f },
                { 0.236f, 0.364f },
            } },
            { {
                { 0.755f, 0.429f },
                { 0.832f, 0.364f },
                { 0.683f, 0.364f },
                { 0.755f, 0.300f },
                { 0.236f, 0.208f },
                { 0.765f, 0.208f },
                { 0.380f, 0.292f },
                { 0.618f, 0.292f },
                { 0.236f, 0.367f },
                { 0.629f, 0.498f },
                { 0.236f, 0.169f },
                { 0.765f, 0.169f },
                { 0.351f, 0.467f },
                { 0.351f, 0.534f },
                { 0.318f, 0.501f },
                { 0.385f, 0.501f },
            } },
        } };

        // The rows of the diagram. Their buttons, glyphs, names and hold notes all come from
        // the mapping; only where each row sits is decided here, to suit the artwork.
        struct Callout {
            gamepad::Action action;
            float y;
        };

        constexpr std::array<Callout, 6> left { {
            { gamepad::Action::Inventory, 40 },
            // Leave a gap so the trigger and bumper share their own inset.
            { gamepad::Action::Lean, 88 },
            { gamepad::Action::BeltRadial, 120 },
            { gamepad::Action::Move, 167 },
            { gamepad::Action::Scope, 199 },
            { gamepad::Action::QuickSlots, 231 },
        } };
        constexpr std::array<Callout, 9> right { {
            { gamepad::Action::MainMenu, 40 },
            { gamepad::Action::Fire, 72 },
            { gamepad::Action::AugRadial, 104 },
            { gamepad::Action::Jump, 136 },
            { gamepad::Action::Crouch, 168 },
            { gamepad::Action::Interact, 200 },
            { gamepad::Action::Reload, 232 },
            { gamepad::Action::Look, 264 },
            { gamepad::Action::Laser, 296 },
        } };

        bool pressedInput(gamepad::Input input)
        {
            return input == gamepad::Input::Press || input == gamepad::Input::LongPress || input == gamepad::Input::Hold;
        }

        // What a row on a button shows: the press or hold of the role the button has now
        // (gamepad::role), the first in the mapping, so play's before the radials'; else its
        // long press, as Lean is on its own. A tap's long press goes in the note under it. A
        // button with no role shows "None".
        const gamepad::Mapping& rowMapping(const gamepad::Mapping& slot)
        {
            static constexpr gamepad::Mapping none { .action = gamepad::Action::Move, .label = "None" };
            if (!pressedInput(slot.control.input)) {
                return slot;
            }
            gamepad::Button role = gamepad::role(slot.control.button);
            if (role == gamepad::ButtonCount) {
                return none;
            }
            const gamepad::Mapping* longPress = nullptr;
            for (const gamepad::Mapping& entry : gamepad::mappings()) {
                if (!gamepad::playAction(entry.action) || entry.control.button != role || !pressedInput(entry.control.input)) {
                    continue;
                }
                if (entry.control.input == gamepad::Input::LongPress) {
                    longPress = longPress ? longPress : &entry;
                    continue;
                }
                // A row with no name of its own, as a paddle's gyro off, shows its action's.
                return entry.label ? entry : gamepad::mapping(entry.action);
            }
            return longPress ? *longPress : none;
        }

        // The note under a row: the action's own hint, or what holding its button does.
        std::string detailOf(const gamepad::Mapping& mapping)
        {
            if (mapping.hint) {
                return mapping.hint;
            }
            const gamepad::Mapping* hold = mapping.control.input == gamepad::Input::Press ? gamepad::longPressOn(mapping.control.button) : nullptr;
            if (!hold || !hold->label) {
                return {};
            }
            std::string label = hold->label;
            label[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(label[0])));
            return "Hold: " + label;
        }

        // A rectangle grown to take in what is added, in the diagram's units.
        struct Box {
            float x0 = 1e9f;
            float y0 = 1e9f;
            float x1 = -1e9f;
            float y1 = -1e9f;

            void add(float ax, float ay, float bx, float by)
            {
                x0 = std::min({ x0, ax, bx });
                y0 = std::min({ y0, ay, by });
                x1 = std::max({ x1, ax, bx });
                y1 = std::max({ y1, ay, by });
            }

            bool empty() const
            {
                return x1 < x0;
            }
        };

        // Where the diagram puts things, measured by a pass that draws nothing.
        struct Shapes {
            Box title;
            std::vector<Box> left;
            std::vector<Box> right;
            std::optional<Box> paddles;
            std::vector<std::pair<Point, Point>> lines;
        };

        // Grows a mask by `r` pixels each way (a square), a row pass and a column pass.
        void dilate(std::vector<uint8_t>& mask, int w, int h, int r)
        {
            std::vector<uint8_t> out(mask.size(), 0);
            for (int y = 0; y < h; y++) {
                int count = 0;
                for (int x = -r; x < w; x++) {
                    if (x + r < w) {
                        count += mask[static_cast<size_t>(y * w + x + r)];
                    }
                    if (x - r - 1 >= 0) {
                        count -= mask[static_cast<size_t>(y * w + x - r - 1)];
                    }
                    if (x >= 0) {
                        out[static_cast<size_t>(y * w + x)] = count > 0;
                    }
                }
            }
            for (int x = 0; x < w; x++) {
                int count = 0;
                for (int y = -r; y < h; y++) {
                    if (y + r < h) {
                        count += out[static_cast<size_t>((y + r) * w + x)];
                    }
                    if (y - r - 1 >= 0) {
                        count -= out[static_cast<size_t>((y - r - 1) * w + x)];
                    }
                    if (y >= 0) {
                        mask[static_cast<size_t>(y * w + x)] = count > 0;
                    }
                }
            }
        }

        // The backdrop behind the diagram, `width` by `height` (the client area), the diagram
        // `k` times its size in it: each column of rows set in dark recessed boxes of one width
        // (rows that would touch sharing one), the paddle row in another, and the traces kept off
        // the title, the callout lines and the controller. Made again when any of that changes.
        UTexture* backdropFor(const Shapes& shapes, UTexture* image, int width, int height, float k)
        {
            static deusex::HeldObject<UTexture> held;
            static std::string heldKey;
            std::string key = std::format("{}x{} {:.3f} {} {}", width, height, k, static_cast<int>(previewStyle), static_cast<const void*>(image));
            auto keyOf = [&](const Box& b) { key += std::format(" {:.1f},{:.1f},{:.1f},{:.1f}", b.x0, b.y0, b.x1, b.y1); };
            keyOf(shapes.title);
            for (const Box& b : shapes.left) {
                keyOf(b);
            }
            for (const Box& b : shapes.right) {
                keyOf(b);
            }
            if (shapes.paddles) {
                keyOf(*shapes.paddles);
            }
            for (const auto& [a, b] : shapes.lines) {
                key += std::format(" {:.0f},{:.0f},{:.0f},{:.0f}", a.x, a.y, b.x, b.y);
            }
            if (held.object && key == heldKey) {
                return held.object;
            }
            if (width < 64 || height < 64) {
                return nullptr;
            }
            float ox = (static_cast<float>(width) - diagramWidth * k) * 0.5f;
            float oy = (static_cast<float>(height) - diagramHeight * k) * 0.5f;
            auto px = [&](float d) { return ox + d * k; };
            auto py = [&](float d) { return oy + d * k; };
            deusex::MenuLayout layout;
            layout.screen = screenName;
            layout.width = width;
            layout.height = height;
            int pad = std::max(3, static_cast<int>(std::lround(4 * k)));
            int touching = std::max(3, static_cast<int>(std::lround(3 * k)));

            struct Rect {
                int x0;
                int y0;
                int x1;
                int y1;
            };

            auto boxed = [&](const Box& b) {
                return Rect { static_cast<int>(std::floor(px(b.x0))) - pad, static_cast<int>(std::floor(py(b.y0))) - pad,
                    static_cast<int>(std::ceil(px(b.x1))) + pad, static_cast<int>(std::ceil(py(b.y1))) + pad };
            };
            auto recess = [&](Rect r) {
                r.x0 = std::max(r.x0, 1);
                r.y0 = std::max(r.y0, 1);
                r.x1 = std::min(r.x1, width - 1);
                r.y1 = std::min(r.y1, height - 1);
                if (r.x1 - r.x0 >= 8 && r.y1 - r.y0 >= 8) {
                    layout.slots.push_back({ deusex::MenuSlot::Kind::TextBox, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0 });
                }
            };
            for (const std::vector<Box>* column : { &shapes.left, &shapes.right }) {
                std::vector<Rect> rows;
                for (const Box& b : *column) {
                    rows.push_back(boxed(b));
                }
                if (rows.empty()) {
                    continue;
                }
                // One width for the column.
                int lo = rows.front().x0;
                int hi = rows.front().x1;
                for (const Rect& r : rows) {
                    lo = std::min(lo, r.x0);
                    hi = std::max(hi, r.x1);
                }
                std::sort(rows.begin(), rows.end(), [](const Rect& a, const Rect& b) { return a.y0 < b.y0; });
                Rect current = rows.front();
                for (size_t i = 1; i < rows.size(); i++) {
                    if (rows[i].y0 - current.y1 < touching) {
                        current.y1 = std::max(current.y1, rows[i].y1);
                    } else {
                        recess({ lo, current.y0, hi, current.y1 });
                        current = rows[i];
                    }
                }
                recess({ lo, current.y0, hi, current.y1 });
            }
            if (shapes.paddles) {
                recess(boxed(*shapes.paddles));
            }

            // What the traces keep clear of: the title and the callouts, a little way off.
            size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
            std::vector<uint8_t> marks(pixels, 0);
            auto mark = [&](int mx, int my) {
                if (mx >= 0 && my >= 0 && mx < width && my < height) {
                    marks[static_cast<size_t>(my * width + mx)] = 1;
                }
            };
            if (!shapes.title.empty()) {
                for (int my = static_cast<int>(py(shapes.title.y0)); my <= static_cast<int>(py(shapes.title.y1)); my++) {
                    for (int mx = static_cast<int>(px(shapes.title.x0)); mx <= static_cast<int>(px(shapes.title.x1)); mx++) {
                        mark(mx, my);
                    }
                }
            }
            for (const auto& [a, b] : shapes.lines) {
                float dx = (b.x - a.x) * k;
                float dy = (b.y - a.y) * k;
                int steps = static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy)))) + 1;
                for (int i = 0; i <= steps; i++) {
                    float t = static_cast<float>(i) / static_cast<float>(steps);
                    mark(static_cast<int>(std::lround(px(a.x) + dx * t)), static_cast<int>(std::lround(py(a.y) + dy * t)));
                }
            }
            dilate(marks, width, height, std::max(2, static_cast<int>(std::lround(2 * k))) + 1);
            // The controller: its art's outline and all inside it.
            if (image && !image->mips.empty() && image->mips[0].data.size() == static_cast<size_t>(image->width()) * image->height() * 4) {
                const TextureMip& mip = image->mips[0];
                int cx0 = static_cast<int>(std::floor(px(184)));
                int cy0 = static_cast<int>(std::floor(py(54)));
                int size = static_cast<int>(std::ceil(252 * k)) + 2;
                std::vector<uint8_t> line(static_cast<size_t>(size) * static_cast<size_t>(size), 0);
                for (int ly = 0; ly < size; ly++) {
                    for (int lx = 0; lx < size; lx++) {
                        float u = (static_cast<float>(cx0 + lx) + 0.5f - px(184)) / (252 * k);
                        float v = (static_cast<float>(cy0 + ly) + 0.5f - py(54)) / (252 * k);
                        if (u < 0 || v < 0 || u >= 1 || v >= 1) {
                            continue;
                        }
                        auto tu = static_cast<size_t>(u * static_cast<float>(mip.width));
                        auto tv = static_cast<size_t>(v * static_cast<float>(mip.height));
                        line[static_cast<size_t>(ly * size + lx)] = mip.data[(tv * mip.width + tu) * 4 + 3] > 64;
                    }
                }
                // Everything the outside cannot reach without crossing the outline is inside.
                std::vector<uint8_t> outside(line.size(), 0);
                std::vector<std::pair<int, int>> stack;
                for (int i = 0; i < size; i++) {
                    for (auto [sx, sy] : { std::pair { i, 0 }, std::pair { i, size - 1 }, std::pair { 0, i }, std::pair { size - 1, i } }) {
                        stack.push_back({ sx, sy });
                    }
                }
                while (!stack.empty()) {
                    auto [sx, sy] = stack.back();
                    stack.pop_back();
                    if (sx < 0 || sy < 0 || sx >= size || sy >= size) {
                        continue;
                    }
                    size_t i = static_cast<size_t>(sy * size + sx);
                    if (outside[i] || line[i]) {
                        continue;
                    }
                    outside[i] = 1;
                    stack.push_back({ sx + 1, sy });
                    stack.push_back({ sx - 1, sy });
                    stack.push_back({ sx, sy + 1 });
                    stack.push_back({ sx, sy - 1 });
                }
                std::vector<uint8_t> controller(pixels, 0);
                for (int ly = 0; ly < size; ly++) {
                    for (int lx = 0; lx < size; lx++) {
                        int mx = cx0 + lx;
                        int my = cy0 + ly;
                        if (!outside[static_cast<size_t>(ly * size + lx)] && mx >= 0 && my >= 0 && mx < width && my < height) {
                            controller[static_cast<size_t>(my * width + mx)] = 1;
                        }
                    }
                }
                dilate(controller, width, height, std::max(3, static_cast<int>(std::lround(4 * k))));
                for (size_t i = 0; i < pixels; i++) {
                    marks[i] = marks[i] || controller[i];
                }
            }
            layout.drawn = std::move(marks);
            deusex::Backdrop art = deusex::generateBackdrop(layout, deusex::backdropSeed(screenName));
            held.object = ui::newTexture(static_cast<uint32_t>(art.width), static_cast<uint32_t>(art.height), std::move(art.bgra));
            heldKey = key;
            return held.object;
        }
    }

    namespace {
        // The row a button is the button of, when it is one of `choice`'s.
        bool buttonOfRow(UObject* button, const char* choice)
        {
            auto* window = cast<extension::UWindow>(button);
            return window && script::isA(window->parent(), choice);
        }

        // A row of the Gamepad screen that only has a button: held, since script names it nowhere.
        void makeActionChoice(deusex::MenuPatch& patch, deusex::HeldObject<UClass>& held, const char* name, const char* text, const char* help)
        {
            UClass* choice = objects::findClass(std::string("DeusEx.") + name);
            if (!choice) {
                UClass* parent = patch.findClass("DeusEx.MenuUIChoiceAction");
                if (!patch.ready()) {
                    return;
                }
                choice = objects::createSubclass(parent, Name(name));
            }
            held.object = choice;
            UObject* defaults = patch.defaults(choice);
            auto* actionText = patch.field<std::string>(defaults, "actionText");
            auto* helpText = patch.field<std::string>(defaults, "helpText");
            auto* action = patch.field<uint8_t>(defaults, "action");
            if (!patch.ready()) {
                return;
            }
            *actionText = text;
            *helpText = help;
            *action = 4; // MA_Custom: ProcessMenuAction does nothing with it.
        }
    }

    void install()
    {
        deusex::MenuPatch patch("the Controller Layout screen");
        UClass* screen = objects::findClass("DeusEx.MenuScreenControllerLayout");
        if (!screen) {
            UClass* parent = patch.findClass("DeusEx.MenuUIScreenWindow");
            if (!patch.ready()) {
                return;
            }
            screen = objects::createSubclass(parent, Name(screenName));
        }
        layoutScreen().object = screen;
        UObject* defaults = patch.defaults(screen);
        auto* title = patch.field<std::string>(defaults, "Title");
        auto* width = patch.field<int32_t>(defaults, "ClientWidth");
        auto* height = patch.field<int32_t>(defaults, "ClientHeight");
        uint8_t* backAction = actionField(patch, defaults, 0, "action");
        uint8_t* backAlign = actionField(patch, defaults, 0, "align");
        if (!patch.ready()) {
            return;
        }
        *title = "Controller Layout";
        *width = static_cast<int32_t>(diagramWidth);
        *height = static_cast<int32_t>(diagramHeight);
        script::setFlag(defaults, "bUsesHelpWindow", false);
        script::setFlag(defaults, "bActionButtonBarActive", true);
        *backAction = 4; // AB_Previous: mouse, Enter and Escape use the game's own navigation.
        *backAlign = 2;  // HALIGN_Right

        static deusex::HeldObject<UClass> layoutRow;
        static deusex::HeldObject<UClass> calibrateRow;
        deusex::MenuPatch layoutPatch("the Controller Layout row");
        makeActionChoice(layoutPatch, layoutRow, layoutChoice, "Controller |&Layout...", "Shows what each of the controller's buttons and sticks does.");
        deusex::MenuPatch calibratePatch("the Calibrate Gyro row");
        makeActionChoice(calibratePatch, calibrateRow, calibrateChoice, calibrateLabel, "Set the controller down on a flat surface, then press to find the gyro's resting point.");
    }

    bool isCalibrateButton(UObject* button)
    {
        return buttonOfRow(button, calibrateChoice);
    }

    void setCalibrateLabel(const char* label)
    {
        auto* client = cast<extension::UWindow>(script::object(extension::topModal(), "winClient"));
        for (auto* choice = client ? client->firstChild() : nullptr; choice; choice = choice->nextSibling()) {
            if (!script::isA(choice, calibrateChoice)) {
                continue;
            }
            UObject* button = script::object(choice, "btnAction");
            UProperty* text = button ? button->cls->findProperty(Name("buttonText")) : nullptr;
            if (text && *reinterpret_cast<std::string*>(button->data + text->offset) != label) {
                vm::Call(button, Name("SetButtonText")).s(label).run();
            }
            return;
        }
    }

    bool buttonPressed(UObject* button)
    {
        UObject* top = extension::topModal();
        UClass* screen = layoutScreen().object;
        if (!screen || !buttonOfRow(button, layoutChoice)) {
            return false;
        }
        previewChoice = static_cast<int>(glyphs::style());
        auto* client = cast<extension::UWindow>(script::object(top, "winClient"));
        for (auto* choice = client ? client->firstChild() : nullptr; choice; choice = choice->nextSibling()) {
            if (script::isA(choice, "MenuChoice_ButtonGlyphs")) {
                previewChoice = std::max(script::integer(choice, "currentValue"), 0);
                break;
            }
        }
        if (auto area = extension::windowArea(extension::rootWindow())) {
            // Sized to the screen: 32 pixels narrower, kept between 580 and 1000 wide, and no
            // taller than the diagram's shape needs, so it is not tiny at large UI scales.
            float width = std::clamp(area->width - 32, 580.0f, 1000.0f);
            float height = std::min(area->height - 80, width * diagramHeight / diagramWidth);
            auto* clientWidth = script::field<int32_t>(screen->defaultObject, "ClientWidth");
            auto* clientHeight = script::field<int32_t>(screen->defaultObject, "ClientHeight");
            if (clientWidth && clientHeight) {
                *clientWidth = static_cast<int32_t>(width);
                *clientHeight = static_cast<int32_t>(height);
            }
        }
        vm::Call(extension::rootWindow(), Name("InvokeMenuScreen")).obj(screen).run();
        return true;
    }

    bool shown()
    {
        return script::isA(extension::topModal(), screenName);
    }

    bool draw(Canvas2D& canvas)
    {
        UObject* top = extension::topModal();
        if (!script::isA(top, screenName)) {
            return false;
        }
        auto area = extension::windowArea(script::object(top, "winClient"));
        if (!area) {
            return true;
        }
        if (previewChoice <= static_cast<int>(glyphs::Style::Switch)) {
            previewStyle = static_cast<glyphs::Style>(previewChoice);
        } else {
            previewStyle = glyphs::padStyle();
        }
        Preview preview;
        float uiScale = extension::uiScale();
        float scale = uiScale * std::min(area->width / diagramWidth, area->height / diagramHeight);
        float x = area->x * uiScale + (area->width * uiScale - diagramWidth * scale) * 0.5f;
        float y = area->y * uiScale + (area->height * uiScale - diagramHeight * scale) * 0.5f;
        ui::Theme theme = ui::theme(hud::viewer(), true);
        LinearColor panel = ui::scaled(theme.panel, 0.1f);
        panel.a = 1;
        LinearColor accent { 0.35f, 0.9f, 0.95f, 1 };
        UFont* font = ui::font("DeusExUI.FontMenuSmall");
        UFont* labelFont = ui::font("DeusExUI.FontMenuHeaders");
        UTexture* image = glyphs::image("pad_layout");
        // The diagram is gone through twice: once to measure where everything goes, for the
        // backdrop behind it, and once to draw it.
        bool paint = false;
        Shapes shapes;
        auto centered = [&](std::string_view label, float py, LinearColor color) {
            float width = textWidth(font, label, 0, scale);
            float textLeft = (diagramWidth * scale - width) * 0.5f;
            shapes.title.add(textLeft / scale, py, (textLeft + width) / scale, py + textHeight(font, scale) / scale);
            if (paint) {
                drawText(canvas, font, label, x + textLeft, y + py * scale, color, BlendStyle::Masked, 0, scale);
            }
        };
        auto line = [&](Point a, Point b) {
            shapes.lines.push_back({ a, b });
            if (paint) {
                canvas.drawLine(std::round(x + a.x * scale), std::round(y + a.y * scale),
                    std::round(x + b.x * scale), std::round(y + b.y * scale), accent);
            }
        };
        auto pass = [&]() {
            centered("GAMEPLAY CONTROLS", 12, accent);
            constexpr std::array<const char*, 3> names { "Xbox Series", "PlayStation 5", "Nintendo Switch Pro" };
            size_t style = static_cast<size_t>(previewStyle);
            centered(names[style], 27, theme.text);
            if (image && paint) {
                canvas.drawTile(image, x + 184 * scale, y + 54 * scale, 252 * scale, 252 * scale,
                    0, 0, static_cast<float>(image->width()), static_cast<float>(image->height()), theme.text, BlendStyle::Masked, true);
            }
            LinearColor detail = ui::scaled(theme.text, 0.75f);
            auto callout = [&](const Callout& entry, bool onRight) {
                // Each row stays on its button, beside it on the art, and says what it does now.
                const gamepad::Mapping& mapping = gamepad::mapping(entry.action);
                const gamepad::Mapping& does = rowMapping(mapping);
                struct {
                    gamepad::Button button;
                    std::string_view label;
                    std::string detail;
                } row { mapping.control.button, does.label ? does.label : "", detailOf(does) };
                float rowY = entry.y;
                // Symmetric PlayStation sticks put its d-pad above, not below, the left stick.
                if (previewStyle == glyphs::Style::PlayStation && !onRight) {
                    if (row.button == gamepad::LeftStick) {
                        rowY += 32;
                    } else if (row.button == gamepad::DPadUp) {
                        rowY -= 64;
                    }
                }
                float lineY = rowY + 8;
                Point point = anchors[style][gamepad::placeOf(row.button)];
                if (row.button == gamepad::DPadUp) {
                    point.y = (point.y + anchors[style][gamepad::DPadDown].y) * 0.5f;
                }
                bool stick = row.button == gamepad::LeftStick || row.button == gamepad::RightStick;
                bool click = stick && mapping.control.input == gamepad::Input::Press;
                bool pair = row.button == gamepad::LeftTrigger || row.button == gamepad::LeftShoulder || row.button == gamepad::RightTrigger || row.button == gamepad::RightShoulder;
                bool pairHead = row.button == gamepad::LeftTrigger || row.button == gamepad::RightTrigger;
                bool face = row.button == gamepad::Y || row.button == gamepad::B || row.button == gamepad::A || row.button == gamepad::X;
                bool faceHead = row.button == gamepad::Y;
                if (pair) {
                    // A trigger and its bumper share one line from the shoulder, forked beside
                    // their icons like a stick's movement and click.
                    Point a = anchors[style][onRight ? gamepad::RightTrigger : gamepad::LeftTrigger];
                    Point b = anchors[style][onRight ? gamepad::RightShoulder : gamepad::LeftShoulder];
                    point = { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
                } else if (face) {
                    // The four face buttons share one line from the middle of their diamond.
                    Point a = anchors[style][gamepad::Y];
                    Point b = anchors[style][gamepad::B];
                    Point c = anchors[style][gamepad::A];
                    Point d = anchors[style][gamepad::X];
                    point = { (a.x + b.x + c.x + d.x) * 0.25f, (a.y + b.y + c.y + d.y) * 0.25f };
                }
                point = { 184 + point.x * 252, 54 + point.y * 252 };
                Point end { onRight ? 457.0f : 163.0f, lineY };
                float direction = onRight ? 1.0f : -1.0f;
                if (stick) {
                    // One line from the stick's center to a bracket outside the controller art, then
                    // a horizontal branch per row. The movement row draws the shared line and
                    // bracket; the click row adds only its own branch. The offset is the gap
                    // between those two rows in the tables above.
                    float clickY = lineY + 32.0f;
                    float middleY = (lineY + clickY) * 0.5f;
                    Point fork { onRight ? 444.0f : 176.0f, middleY };
                    Point out { onRight ? fork.x - 22.0f : fork.x + 22.0f, middleY };
                    if (!click) {
                        line(point, out);
                        line(out, fork);
                        line({ fork.x, lineY }, { fork.x, clickY });
                    }
                    line({ fork.x, lineY }, end);
                } else if (pair) {
                    float pairTop = pairHead ? lineY : lineY - 32.0f;
                    float pairBottom = pairTop + 32.0f;
                    float middleY = (pairTop + pairBottom) * 0.5f;
                    Point fork { onRight ? 444.0f : 176.0f, middleY };
                    Point out { onRight ? fork.x - 22.0f : fork.x + 22.0f, middleY };
                    if (pairHead) {
                        line(point, out);
                        line(out, fork);
                        line({ fork.x, pairTop }, { fork.x, pairBottom });
                    }
                    line({ fork.x, lineY }, end);
                } else if (face) {
                    int index = row.button == gamepad::Y ? 0 : row.button == gamepad::B ? 1
                        : row.button == gamepad::A                                      ? 2
                                                                                        : 3;
                    float faceTop = lineY - static_cast<float>(index) * 32.0f;
                    float faceBottom = faceTop + 96.0f;
                    float middleY = (faceTop + faceBottom) * 0.5f;
                    Point fork { 444.0f, middleY };
                    Point out { fork.x - 22.0f, middleY };
                    if (faceHead) {
                        line(point, out);
                        line(out, fork);
                        line({ fork.x, faceTop }, { fork.x, faceBottom });
                    }
                    line({ fork.x, lineY }, end);
                } else if (row.button == gamepad::DPadUp) {
                    Point below { point.x, std::max(point.y + 12, lineY - 35) };
                    Point bend { point.x + direction * 33, lineY };
                    line(point, below);
                    line(below, bend);
                    line(bend, end);
                } else {
                    Point bend { onRight ? std::max(point.x + 20, 414.0f) : std::min(point.x - 20, 216.0f), lineY };
                    line(point, bend);
                    line(bend, end);
                }
                if (!click && (!pair || pairHead) && (!face || faceHead) && paint) {
                    canvas.drawRect(x + (point.x - 1) * scale, y + (point.y - 1) * scale, 2 * scale, 2 * scale, accent, BlendStyle::Normal);
                }
                float glyphX = onRight ? 464.0f : 140.0f;
                if (paint) {
                    glyphs::draw(canvas, glyphs::image(mapping.control), x + glyphX * scale, y + rowY * scale, 16 * scale, theme.text);
                }
                float textEdge = onRight ? 488.0f : 131.0f;
                float labelX = x + textEdge * scale - (onRight ? 0 : textWidth(labelFont, row.label, 0, scale));
                float detailX = x + textEdge * scale - (onRight ? 0 : textWidth(font, row.detail, 0, scale));
                // Center the text block on the button icon: one line on its own, two lines as a
                // block, so a row with a hold action stays balanced with the rows around it.
                float labelHeight = textHeight(labelFont, scale);
                float detailHeight = textHeight(font, scale);
                float gap = row.detail.empty() ? 0.0f : std::max(0.0f, 13.0f * scale - labelHeight);
                float blockHeight = labelHeight + (row.detail.empty() ? 0.0f : gap + detailHeight);
                float textTop = y + (rowY + 8.0f) * scale - blockHeight * 0.5f;
                // The row's glyph and text, for the box the backdrop sets them in.
                float textWide = std::max(textWidth(labelFont, row.label, 0, scale), row.detail.empty() ? 0.0f : textWidth(font, row.detail, 0, scale)) / scale;
                Box box;
                box.add(glyphX, rowY, glyphX + 16, rowY + 16);
                box.add(onRight ? textEdge : textEdge - textWide, (textTop - y) / scale, onRight ? textEdge + textWide : textEdge, (textTop - y + blockHeight) / scale);
                (onRight ? shapes.right : shapes.left).push_back(box);
                if (!paint) {
                    return;
                }
                drawText(canvas, labelFont, row.label, labelX, textTop, theme.text, BlendStyle::Masked, 0, scale);
                if (!row.detail.empty()) {
                    drawText(canvas, font, row.detail, detailX, textTop + labelHeight + gap, detail, BlendStyle::Masked, 0, scale);
                }
            };
            for (const Callout& row : left) {
                callout(row, false);
            }
            for (const Callout& row : right) {
                callout(row, true);
            }
            // The back paddles are not on the art: a row of them along the bottom, for a pad that
            // has them or a layout that gives them a role.
            bool remapped = false;
            for (gamepad::Button paddle : { gamepad::RightPaddle1, gamepad::LeftPaddle1, gamepad::RightPaddle2, gamepad::LeftPaddle2 }) {
                remapped = remapped || gamepad::role(paddle) != paddle;
            }
            if (paddlesShown || remapped) {
                struct Entry {
                    gamepad::Button paddle;
                    std::string label;
                };

                std::vector<Entry> entries;
                float total = 0;
                constexpr float glyphGap = 6;
                constexpr float entryGap = 18;
                for (gamepad::Button paddle : { gamepad::LeftPaddle1, gamepad::LeftPaddle2, gamepad::RightPaddle1, gamepad::RightPaddle2 }) {
                    gamepad::Mapping slot { .action = gamepad::Action::GyroOff, .control = { paddle, gamepad::Input::Hold } };
                    const gamepad::Mapping& does = rowMapping(slot);
                    std::string label = does.label ? does.label : "";
                    if (std::string note = detailOf(does); !note.empty()) {
                        label += " (" + note + ")";
                    }
                    total += 16 + glyphGap + textWidth(font, label, 0, 1) + entryGap;
                    entries.push_back({ paddle, std::move(label) });
                }
                total -= entryGap;
                float rowX = (diagramWidth - total) * 0.5f;
                constexpr float rowY = 330;
                Box box;
                box.add(rowX, rowY, rowX + total, rowY + 16);
                shapes.paddles = box;
                for (const Entry& entry : entries) {
                    if (paint) {
                        glyphs::draw(canvas, glyphs::image(entry.paddle), x + rowX * scale, y + rowY * scale, 16 * scale, theme.text);
                    }
                    rowX += 16 + glyphGap;
                    float textY = y + (rowY + 8) * scale - textHeight(font, scale) * 0.5f;
                    if (paint) {
                        drawText(canvas, font, entry.label, x + rowX * scale, textY, theme.text, BlendStyle::Masked, 0, scale);
                    }
                    rowX += textWidth(font, entry.label, 0, 1) + entryGap;
                }
            }
        };
        pass();
        canvas.drawRect(area->x * uiScale, area->y * uiScale, area->width * uiScale, area->height * uiScale, panel, BlendStyle::Normal);
        // The backdrop, in the client area's own pixels as the other menus' art is, drawn up to
        // the UI scale; tinted with the menu background (theme.panel is it at 0.2), its cut-outs
        // showing the panel.
        float k = std::min(area->width / diagramWidth, area->height / diagramHeight);
        int width = static_cast<int>(std::lround(area->width));
        int height = static_cast<int>(std::lround(area->height));
        if (UTexture* backdrop = backdropFor(shapes, image, width, height, k)) {
            LinearColor tint = ui::scaled(theme.panel, 5.0f);
            tint = { std::min(tint.r, 1.0f), std::min(tint.g, 1.0f), std::min(tint.b, 1.0f), 1 };
            canvas.drawTile(backdrop, area->x * uiScale, area->y * uiScale, static_cast<float>(width) * uiScale, static_cast<float>(height) * uiScale,
                0, 0, static_cast<float>(width), static_cast<float>(height), tint, BlendStyle::Masked, false);
        }
        paint = true;
        pass();
        return true;
    }

    void showPaddles(bool shown)
    {
        paddlesShown = shown;
    }
}

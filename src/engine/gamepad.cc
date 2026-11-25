#include "engine/gamepad.h"

#include "engine/input.h"

#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <vector>

namespace dx {
    namespace gamepad {
        namespace {
            // The whole mapping. Each action's first row is its button; see Mapping.
            constexpr Mapping table[] = {
                // Play. A Press with a command is the default binding of the button's key, so
                // a rebind in the controls menu changes what it does; the rest act natively.
                { .action = Action::Move, .control = { LeftStick, Input::Stick }, .label = "Move" },
                { .action = Action::Look, .control = { RightStick, Input::Stick }, .label = "Look" },
                // Using things is the most common action, so it gets the main button; jumping
                // is rare enough to sit on Y.
                { .action = Action::Interact, .control = { A }, .command = "ParseRightClick", .label = "Interact" },
                // Crouch stays on until B is pressed again. In play GamepadInput::crouch does it
                // rather than this binding, whose Toggle can leave the player stuck crouched.
                { .action = Action::Crouch, .control = { B }, .command = "Toggle bDuck", .label = "Toggle crouch" },
                // With empty hands, a tap draws again what was last put away.
                { .action = Action::Reload, .control = { X }, .command = "ReloadWeapon", .label = "Reload / redraw" },
                { .action = Action::PutAway, .control = { X, Input::LongPress }, .command = "PutInHand", .holdSeconds = 0.35f, .label = "Put away" },
                { .action = Action::Jump, .control = { Y }, .command = "Jump", .label = "Jump" },
                { .action = Action::Inventory, .control = { View }, .command = "ShowInventoryWindow", .label = "Inventory" },
                // Saving and loading ask for a longer hold; loading asks first.
                { .action = Action::QuickSave, .control = { Menu, Input::LongPress }, .command = "QuickSave", .holdSeconds = 0.8f, .label = "Quick save" },
                { .action = Action::MainMenu, .control = { Menu }, .command = "ShowMainMenu", .label = "Main menu" },
                { .action = Action::QuickLoad, .control = { View, Input::LongPress }, .command = "QuickLoad", .holdSeconds = 0.8f, .label = "Quick load" },
                // Walking is a light push of the Move stick rather than a toggle, which leaves
                // its click free for the scope.
                { .action = Action::Scope, .control = { LeftStick }, .command = "ToggleScope", .label = "Scope / zoom" },
                // The laser toggles on a quick tap; held, the right stick turns gyro aiming off
                // from the moment it is pressed, so a hold to move the controller back leaves
                // the laser as it was.
                { .action = Action::Laser, .control = { RightStick }, .command = "ToggleLaser", .label = "Laser sight" },
                { .action = Action::GyroOff, .control = { RightStick, Input::LongPress }, .holdSeconds = 0.2f, .label = "Gyro off", .hint = "Hold" },
                // The back paddles do it too, as long as they are held.
                { .action = Action::GyroOff, .control = { RightPaddle1, Input::Hold }, .shown = false },
                { .action = Action::GyroOff, .control = { LeftPaddle1, Input::Hold }, .shown = false },
                { .action = Action::GyroOff, .control = { RightPaddle2, Input::Hold }, .shown = false },
                { .action = Action::GyroOff, .control = { LeftPaddle2, Input::Hold }, .shown = false },
                // Held, the Move stick leans instead, as far as it is pushed; see leanKey.
                { .action = Action::Lean, .control = { LeftTrigger, Input::LongPress }, .holdSeconds = 0.4f, .label = "Lean", .hint = "Hold + left stick" },
                { .action = Action::Fire, .control = { RightTrigger }, .command = "ParseLeftClick|Fire", .label = "Fire / use item" },
                { .action = Action::BeltRadial, .control = { LeftShoulder, Input::Hold }, .label = "Belt Items", .hint = "Hold" },
                { .action = Action::AugRadial, .control = { RightShoulder, Input::Hold }, .label = "Augmentations", .hint = "Hold" },
                { .action = Action::QuickSlots, .control = { DPadUp, Input::DPad }, .label = "Quick slots" },

                // While a radial is open: either stick picks a slice and letting go of the
                // opening bumper closes it.
                { .action = Action::RadialUse, .control = { A } },
                // A d-pad direction puts the slice on that quick slot.
                { .action = Action::RadialAssign, .control = { DPadUp, Input::DPad } },
                // In the belt radial, for the equipped weapon whatever slice is highlighted.
                { .action = Action::SwitchAmmo, .control = { X } },

                // Every menu. The buttons that open menus also back out of them.
                { .action = Action::MenuSelect, .control = { A } },
                { .action = Action::MenuBack, .control = { B } },
                { .action = Action::MenuBack, .control = { View }, .shown = false },
                { .action = Action::MenuBack, .control = { Menu }, .shown = false },
                { .action = Action::MenuNavigate, .control = { LeftStick, Input::Stick } },
                { .action = Action::MenuNavigate, .control = { DPadUp, Input::DPad } },
                { .action = Action::MenuScroll, .control = { RightStick, Input::StickVertical } },
                { .action = Action::PreviousTab, .control = { LeftShoulder } },
                { .action = Action::NextTab, .control = { RightShoulder } },

                { .action = Action::ChangeAmmo, .control = { RightTrigger } },
                { .action = Action::Equip, .control = { Y } },
                { .action = Action::Drop, .control = { LeftTrigger } },
                { .action = Action::MoveItem, .control = { X } },
                { .action = Action::PlaceItem, .control = { A } },
                { .action = Action::PlaceItem, .control = { X } },
                { .action = Action::ClearSlot, .control = { X } },
                { .action = Action::Sort, .control = { RightStick } },

                { .action = Action::ActivateAug, .control = { X } },
                { .action = Action::UpgradeAug, .control = { Y } },
                { .action = Action::UseCell, .control = { LeftTrigger } },
                { .action = Action::UpgradeSkill, .control = { Y } },
                { .action = Action::DowngradeSkill, .control = { X } },
                { .action = Action::HealAll, .control = { X } },
                { .action = Action::DeleteNote, .control = { LeftTrigger } },

                // Once the Ice Breaker is through, Hack's button goes into the computer.
                { .action = Action::Hack, .control = { Y } },
                { .action = Action::Autologin, .control = { X } },
            };

            struct ButtonInfo {
                const char* name;
                const char* glyph;
                // The glyphs for moving a stick, rather than clicking it.
                const char* stickGlyph = nullptr;
                const char* verticalGlyph = nullptr;
            };

            constexpr std::array<ButtonInfo, ButtonCount> buttons { {
                { "a", "pad_a" },
                { "b", "pad_b" },
                { "x", "pad_x" },
                { "y", "pad_y" },
                { "lb", "pad_lb" },
                { "rb", "pad_rb" },
                { "view", "pad_view" },
                { "menu", "pad_menu" },
                { "ls", "pad_ls_click", "pad_ls" },
                { "rs", "pad_rs_click", "pad_rs", "pad_rs_updown" },
                { "lt", "pad_lt" },
                { "rt", "pad_rt" },
                { "up", "pad_dpad_up" },
                { "down", "pad_dpad_down" },
                { "left", "pad_dpad_left" },
                { "right", "pad_dpad_right" },
                { "r4", "pad_r4" },
                { "l4", "pad_l4" },
                { "r5", "pad_r5" },
                { "l5", "pad_l5" },
            } };

            constexpr std::array<Button, 4> clockwiseDPad { DPadUp, DPadRight, DPadDown, DPadLeft };

            bool isDPad(Button button)
            {
                return clockwise(button) >= 0;
            }

            constexpr uint8_t joy(Button button)
            {
                return static_cast<uint8_t>(IK_Joy1 + static_cast<int>(button));
            }

            // Analog sticks feed Axis commands as value * deltaSeconds, the same scale as a held
            // key, so Speed= matches the keyboard aliases at full deflection. The Move stick
            // feeds JoyX/JoyY and the Look stick JoyU/JoyV; the triggers act as buttons.
            constexpr std::array<Binding, 8> analogBindings { {
                { IK_JoyX, "Axis aStrafe Speed=+300.0" },
                { IK_JoyY, "Axis aBaseY Speed=-300.0" },
                { IK_JoyU, "Axis aBaseX Speed=+250.0" },
                { IK_JoyV, "Axis aLookUp Speed=-150.0" },
                { IK_JoyZ, "" },
                { IK_JoyR, "" },
                { leanKey, "Axis aExtra0 Speed=+0.05" },
                { walkKey, "Button bRun" },
            } };
        }

        std::span<const Mapping> mappings()
        {
            return table;
        }

        const Mapping& mapping(Action action)
        {
            for (const Mapping& entry : table) {
                if (entry.action == action) {
                    return entry;
                }
            }
            static constexpr Mapping none = [] {
                Mapping mapping {};
                mapping.action = Action::Move;
                return mapping;
            }();
            return none;
        }

        Control control(Action action)
        {
            return mapping(action).control;
        }

        Button button(Action action)
        {
            return control(action).button;
        }

        Control alternative(Action action)
        {
            bool first = true;
            for (const Mapping& entry : table) {
                if (entry.action != action) {
                    continue;
                }
                if (!first && entry.shown) {
                    return entry.control;
                }
                first = false;
            }
            return {};
        }

        bool triggers(Action action, Button button)
        {
            return std::any_of(std::begin(table), std::end(table), [action, button](const Mapping& entry) {
                if (entry.action != action) {
                    return false;
                }
                switch (entry.control.input) {
                case Input::Press:
                case Input::LongPress:
                case Input::Hold:
                    return entry.control.button == button;
                case Input::DPad:
                    return isDPad(button);
                case Input::Stick:
                case Input::StickVertical:
                    break;
                }
                return false;
            });
        }

        const Mapping* longPressOn(Button button)
        {
            for (const Mapping& entry : table) {
                if (entry.control.input == Input::LongPress && entry.control.button == button) {
                    return &entry;
                }
            }
            return nullptr;
        }

        Button placeOf(Button button, bool nintendo)
        {
            if (nintendo && button == A) {
                return B;
            }
            if (nintendo && button == B) {
                return A;
            }
            return button;
        }

        Button fromSdl(uint8_t sdlButton, bool nintendo)
        {
            switch (sdlButton) {
            case SDL_GAMEPAD_BUTTON_SOUTH:
                return placeOf(A, nintendo);
            case SDL_GAMEPAD_BUTTON_EAST:
                return placeOf(B, nintendo);
            case SDL_GAMEPAD_BUTTON_WEST:
                return X;
            case SDL_GAMEPAD_BUTTON_NORTH:
                return Y;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                return LeftShoulder;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                return RightShoulder;
            case SDL_GAMEPAD_BUTTON_BACK:
            // A PlayStation pad's touchpad click does the same as its Create or Share button,
            // and its prompts show the touchpad (see glyphs::image).
            case SDL_GAMEPAD_BUTTON_TOUCHPAD:
                return View;
            case SDL_GAMEPAD_BUTTON_START:
                return Menu;
            case SDL_GAMEPAD_BUTTON_LEFT_STICK:
                return LeftStick;
            case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
                return RightStick;
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                return DPadUp;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                return DPadDown;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                return DPadLeft;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                return DPadRight;
            case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:
                return RightPaddle1;
            case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:
                return LeftPaddle1;
            case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2:
                return RightPaddle2;
            case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2:
                return LeftPaddle2;
            default:
                return ButtonCount;
            }
        }

        namespace {
            Layout& currentLayout()
            {
                static Layout layout = defaultLayout();
                return layout;
            }
        }

        Layout defaultLayout()
        {
            Layout layout {};
            for (int i = 0; i < ButtonCount; i++) {
                layout[static_cast<size_t>(i)] = static_cast<Button>(i);
            }
            return layout;
        }

        Layout parseLayout(std::span<const std::string> values)
        {
            Layout layout = defaultLayout();
            for (size_t i = 0; i < values.size() && i < layout.size(); i++) {
                std::string name = values[i];
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name == "none") {
                    layout[i] = ButtonCount;
                } else if (Button button = buttonFromName(name); button != ButtonCount) {
                    layout[i] = button;
                }
            }
            return layout;
        }

        void setLayout(const Layout& layout)
        {
            currentLayout() = layout;
        }

        Button role(Button button)
        {
            return button < ButtonCount ? currentLayout()[button] : ButtonCount;
        }

        Button playedBy(Button which)
        {
            if (which >= ButtonCount) {
                return ButtonCount;
            }
            // Itself where it keeps its role, so a role also given to another shows its own.
            if (currentLayout()[which] == which) {
                return which;
            }
            for (int i = 0; i < ButtonCount; i++) {
                if (currentLayout()[static_cast<size_t>(i)] == which) {
                    return static_cast<Button>(i);
                }
            }
            return ButtonCount;
        }

        bool playAction(Action action)
        {
            return action <= Action::SwitchAmmo;
        }

        Control shown(Action action, Control control)
        {
            bool pressed = control.input == Input::Press || control.input == Input::LongPress || control.input == Input::Hold;
            if (!playAction(action) || !pressed || control.button >= ButtonCount) {
                return control;
            }
            control.button = playedBy(control.button);
            return control;
        }

        Control shownControl(Action action)
        {
            return shown(action, control(action));
        }

        const char* buttonName(Button button)
        {
            return button < ButtonCount ? buttons[button].name : nullptr;
        }

        Button buttonFromName(std::string_view name)
        {
            for (size_t i = 0; i < buttons.size(); i++) {
                if (name == buttons[i].name) {
                    return static_cast<Button>(i);
                }
            }
            return ButtonCount;
        }

        const char* glyphName(Button button)
        {
            return button < ButtonCount ? buttons[placeOf(button)].glyph : nullptr;
        }

        const char* glyphName(Control control)
        {
            if (control.button >= ButtonCount) {
                return nullptr;
            }
            const ButtonInfo& info = buttons[placeOf(control.button)];
            switch (control.input) {
            case Input::Press:
            case Input::LongPress:
            case Input::Hold:
                return info.glyph;
            case Input::Stick:
                return info.stickGlyph;
            case Input::StickVertical:
                return info.verticalGlyph;
            case Input::DPad:
                return "pad_dpad";
            }
            return nullptr;
        }

        int clockwise(Button button)
        {
            auto found = std::find(clockwiseDPad.begin(), clockwiseDPad.end(), button);
            return found == clockwiseDPad.end() ? -1 : static_cast<int>(found - clockwiseDPad.begin());
        }

        Button dpadClockwise(int index)
        {
            return index >= 0 && index < static_cast<int>(clockwiseDPad.size()) ? clockwiseDPad[static_cast<size_t>(index)] : ButtonCount;
        }

        uint8_t buttonKey(Button button)
        {
            constexpr std::array<uint8_t, 4> pov { IK_JoyPovUp, IK_JoyPovRight, IK_JoyPovDown, IK_JoyPovLeft };
            if (int direction = clockwise(button); direction >= 0) {
                return pov[static_cast<size_t>(direction)];
            }
            return button < RightPaddle1 ? joy(button) : static_cast<uint8_t>(IK_None);
        }

        uint8_t menuKey(Button button)
        {
            if (triggers(Action::MenuSelect, button)) {
                return IK_Enter;
            }
            if (triggers(Action::MenuBack, button)) {
                return IK_Escape;
            }
            constexpr std::array<uint8_t, 4> arrows { IK_Up, IK_Right, IK_Down, IK_Left };
            if (triggers(Action::MenuNavigate, button) && clockwise(button) >= 0) {
                return arrows[static_cast<size_t>(clockwise(button))];
            }
            return IK_None;
        }

        std::span<const Binding> defaultBindings()
        {
            // Every button key is bound, to nothing when the button acts natively, so the
            // stock joystick bindings never reach a pad.
            static const std::vector<Binding> bindings = [] {
                std::vector<Binding> out(analogBindings.begin(), analogBindings.end());
                for (int i = 0; i < ButtonCount; i++) {
                    auto which = static_cast<Button>(i);
                    if (buttonKey(which) == IK_None) {
                        continue;
                    }
                    const char* command = "";
                    for (const Mapping& entry : table) {
                        if (entry.control.input == Input::Press && entry.control.button == which && entry.command) {
                            command = entry.command;
                            break;
                        }
                    }
                    out.push_back({ buttonKey(which), command });
                }
                return out;
            }();
            return bindings;
        }

        Stick shapeStick(float x, float y, float deadZone, float exponent, float outerZone)
        {
            float magnitude = std::sqrt(x * x + y * y);
            if (magnitude <= deadZone || deadZone >= outerZone) {
                return {};
            }
            float travel = std::min((magnitude - deadZone) / (outerZone - deadZone), 1.0f);
            float scale = std::pow(travel, exponent) / magnitude;
            return { x * scale, y * scale };
        }

        namespace {
            constexpr float flickPi = 3.14159265f;

            // An angle brought into -pi..pi.
            float wrapAngle(float angle)
            {
                return std::remainder(angle, 2.0f * flickPi);
            }

            // How far through a flick is turned at a part `t` of its time: easing out, so it
            // starts at once and settles.
            float flickEase(float t)
            {
                t = std::clamp(t, 0.0f, 1.0f);
                float rest = 1.0f - t;
                return 1.0f - rest * rest;
            }
        }

        float FlickStick::update(float x, float y, float seconds)
        {
            float turn = 0;
            float magnitude = std::sqrt(x * x + y * y);
            // Up is ahead, and right turns right.
            float angle = std::atan2(x, -y);
            sinceChange += std::max(seconds, 0.0f);
            if (!held && magnitude >= rim) {
                held = true;
                lastAngle = angle;
                flick = angle;
                elapsed = 0;
                sinceChange = turnGap;
            } else if (held && magnitude >= release) {
                float change = wrapAngle(angle - lastAngle);
                if (change != 0) {
                    // Spread over the usual time between changes, not this one's: a change seen
                    // a frame after the last stands for one report however long the frame, and
                    // one seen after a frame with none for one report too.
                    if (sinceChange < turnGap) {
                        changeInterval = changeInterval > 0 ? changeInterval + (sinceChange - changeInterval) * 0.1f : sinceChange;
                    }
                    float span = changeInterval > 0 ? changeInterval : seconds;
                    if (span > 0) {
                        rotation.add({ 0, change / span, 0 }, span, 0);
                    } else {
                        turn += change;
                    }
                    lastAngle = angle;
                    sinceChange = 0;
                }
            } else if (held) {
                held = false;
            }
            // What is still queued of a turn comes out after the stick lets go too: all of it.
            turn += rotation.take(seconds).yaw;
            if (elapsed < flickSeconds && seconds > 0) {
                float before = flickEase(elapsed / flickSeconds);
                elapsed += seconds;
                turn += flick * (flickEase(elapsed / flickSeconds) - before);
            }
            return turn;
        }

        void FlickStick::reset()
        {
            *this = {};
        }

        bool triggerHeld(float value, bool wasHeld)
        {
            return wasHeld ? value > 0.3f : value > 0.5f;
        }
    }
}

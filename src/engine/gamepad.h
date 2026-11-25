#pragma once

#include "core/platform.h"
#include "engine/gyro.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dx {
    namespace gamepad {
        // The controller mapping. Everything that asks which button does what, or which glyph
        // stands for an action, asks here: the bindings sent to the game, the native shortcuts
        // (radials, quick slots, holds), the menus' shortcuts, the prompts and the layout
        // diagram. No other code names a button for an action.

        // Physical buttons, named by the Xbox layout and positional on every pad: A is the
        // bottom face button. Face, shoulder, menu and stick buttons occupy IK_Joy1..IK_Joy10,
        // the triggers act as IK_Joy11/IK_Joy12 when pulled, and the d-pad reports as the POV hat.
        // The back paddles of pads that have them (Xbox Elite, DualSense Edge, Steam Deck) come
        // last, in SDL's order and named as the Steam Deck's: R4 and L4 the upper pair (Elite P1
        // and P3), R5 and L5 the lower (P2 and P4). They have no key and act only natively.
        enum Button : uint8_t {
            A,
            B,
            X,
            Y,
            LeftShoulder,
            RightShoulder,
            View,
            Menu,
            LeftStick,
            RightStick,
            LeftTrigger,
            RightTrigger,
            DPadUp,
            DPadDown,
            DPadLeft,
            DPadRight,
            RightPaddle1,
            LeftPaddle1,
            RightPaddle2,
            LeftPaddle2,
            ButtonCount,
        };

        // Remapping, from [Ambrosia.Gamepad] in DeusEx.ini: the button whose role each button
        // takes in play, or ButtonCount for none. By default each is itself. Menus do not
        // follow it.
        using Layout = std::array<Button, ButtonCount>;
        Layout defaultLayout();
        // The layout from each button's ini value, in button order: a button's name (any
        // case), "none", or empty, or a name it does not know, for itself.
        Layout parseLayout(std::span<const std::string> values);
        void setLayout(const Layout& layout);
        Button role(Button button);
        // The button that plays a role in play: itself where it keeps it, else the first that
        // has it, or ButtonCount.
        Button playedBy(Button role);

        // Everything the pad does, in play, in the radials and in menus.
        enum class Action : uint8_t {
            // Play.
            Move,
            Look,
            Interact,
            Crouch,
            Reload,
            PutAway,
            Jump,
            Inventory,
            QuickSave,
            MainMenu,
            QuickLoad,
            Scope,
            Laser,
            Lean,
            Fire,
            BeltRadial,
            AugRadial,
            QuickSlots,
            // Held, gyro aiming stops at once, so the controller can be moved back to a
            // comfortable place without turning the view, as a mouse is lifted off its pad.
            // The back paddles do it too, on pads that have them.
            GyroOff,
            // While a radial is open.
            RadialUse,
            RadialAssign,
            SwitchAmmo,
            // Every menu.
            MenuSelect,
            MenuBack,
            MenuNavigate,
            MenuScroll,
            PreviousTab,
            NextTab,
            // The inventory screen.
            ChangeAmmo,
            Equip,
            Drop,
            MoveItem,
            PlaceItem,
            ClearSlot,
            Sort,
            // The other persona screens.
            ActivateAug,
            UpgradeAug,
            UseCell,
            UpgradeSkill,
            // The new game screen's skills.
            DowngradeSkill,
            HealAll,
            DeleteNote,
            // Computers and keypads.
            Hack,
            Autologin,
        };

        // How an action is performed with its button.
        enum class Input : uint8_t {
            // A press. A button with a LongPress as well does this on a tap.
            Press,
            // Held past holdSeconds; letting go sooner is a tap, the button's Press.
            LongPress,
            // Held for as long as the action lasts, such as a radial staying open.
            Hold,
            // Moving the stick, rather than clicking it.
            Stick,
            // Moving the stick up and down.
            StickVertical,
            // Any d-pad direction, each doing the action its own way.
            DPad,
        };

        struct Control {
            Button button = ButtonCount;
            Input input = Input::Press;
        };

        // One way to perform an action. An action may have several; the first is its button,
        // the second, if shown, is drawn beside it as an alternative ("A / X").
        struct Mapping {
            Action action;
            Control control {};
            // The console command a Press sends through the bindings, or a LongPress runs.
            const char* command = nullptr;
            float holdSeconds = 0;
            // The action's name on the controller layout diagram, and a note under it.
            const char* label = nullptr;
            const char* hint = nullptr;
            // False for a way that works but is not drawn, as View and Menu also back out of menus.
            bool shown = true;
        };

        std::span<const Mapping> mappings();
        // An action's first, primary way.
        const Mapping& mapping(Action action);
        Control control(Action action);
        Button button(Action action);
        // Its second shown way, if any, with button ButtonCount otherwise.
        Control alternative(Action action);
        // Whether an action is one of play's, the radials' included, which remapping moves.
        bool playAction(Action action);
        // The control as the prompts and the layout diagram draw it: for a play action's button,
        // the button that plays that role now (playedBy).
        Control shown(Action action, Control control);
        Control shownControl(Action action);
        // Whether pressing this button performs the action: Press, LongPress, Hold, or any
        // d-pad direction for DPad. Moving a stick is not a press.
        bool triggers(Action action, Button button);
        // The long press sharing a button with a tap, or null.
        const Mapping* longPressOn(Button button);

        // Nintendo's convention for the two main face buttons: the one labelled A, on the right,
        // does what A does here (interact, menu select) and the one labelled B, at the bottom,
        // what B does (crouch, menu back). X and Y keep their positions either way. The Switch
        // build follows it (platform::nintendoFaceButtons); elsewhere A and B sit where an Xbox
        // pad has them.
        // Where a button is on the pad, as the Xbox button in that place: itself, or A and B
        // swapped under Nintendo's convention. Prompts and the layout diagram draw that place's
        // glyph.
        Button placeOf(Button button, bool nintendo = platform::nintendoFaceButtons());
        // SDL's gamepad button, which SDL names by place, to ours, or ButtonCount.
        Button fromSdl(uint8_t sdlButton, bool nintendo = platform::nintendoFaceButtons());
        // The short names test scripts and the remapping use: "a", "lb", "ls", "up", "r4" and so on.
        const char* buttonName(Button button);
        Button buttonFromName(std::string_view name);
        // The Xbox glyph names: a button's own ("pad_x", "pad_ls_click", "pad_dpad_up"), or a
        // control's ("pad_ls" for moving the left stick, "pad_dpad" for the whole d-pad).
        const char* glyphName(Button button);
        const char* glyphName(Control control);
        // The d-pad clockwise from up, as the quick slots and radial slices go: 0 up, 1 right,
        // 2 down, 3 left. Returns -1 for any other button.
        int clockwise(Button button);
        Button dpadClockwise(int index);

        // The key a button sends to the bindings, IK_None for the paddles.
        uint8_t buttonKey(Button button);
        // What a button sends while a menu is open: MenuSelect is Enter, MenuBack Escape and
        // MenuNavigate's d-pad the arrow keys. Returns IK_None for buttons with no UI role.
        uint8_t menuKey(Button button);

        struct Binding {
            uint8_t key;
            const char* command;
        };

        std::span<const Binding> defaultBindings();
        // Spare joystick keys: one carries the Move stick as a lean axis while Lean is held,
        // the other the walk input while the Move stick is only partly pushed.
        inline constexpr uint8_t leanKey = 200 + 12;
        inline constexpr uint8_t walkKey = 200 + 13;

        struct Stick {
            float x = 0;
            float y = 0;
        };

        // Applies a radial dead zone, rescales the travel between it and the outer zone to 0..1
        // and raises the magnitude to `exponent`, keeping the direction.
        Stick shapeStick(float x, float y, float deadZone, float exponent, float outerZone = 1.0f);

        // Flick stick (gyrowiki.jibbsmart.com, "Flick Stick"): pushing the stick all the way turns
        // the view to face the way it points, ahead being up, over flickSeconds; turning it about
        // the rim meanwhile turns the view as far. Below the rim it does nothing, leaving
        // fine aim and looking up and down to the gyro.
        //
        // The stick is read once a frame, but the controller reports at its own rate, so a
        // steady turn about the rim shows as no change some frames and two reports' worth on
        // others. Each change is taken as a rate over the time since the last one and paid out
        // evenly (gyro::Pacer), as the gyro's samples are, so the turn does not stutter.
        class FlickStick {
        public:
            static constexpr float flickSeconds = 0.1f;
            // How far out the stick must go to flick, and how far back it may come before it
            // lets go.
            static constexpr float rim = 0.9f;
            static constexpr float release = 0.8f;

            // The stick as SDL gives it (y down), for a tick of `seconds`. Returns the turn for
            // this tick in radians, right positive.
            float update(float x, float y, float seconds);
            void reset();

            // The longest gap between two changes of the stick's angle that still counts as one
            // turn: a change after a longer one is a turn starting, spread over its own frame.
            static constexpr float turnGap = 0.1f;

        private:
            bool held = false;
            float lastAngle = 0;
            // The flick being paid out: its whole angle and how far through it is.
            float flick = 0;
            float elapsed = flickSeconds;
            // The turn about the rim, the time since the stick's angle last changed, and the
            // usual time between changes, averaged: the controller's report interval while the
            // stick turns steadily.
            gyro::Pacer rotation;
            float sinceChange = turnGap;
            float changeInterval = 0;
        };

        // Trigger to button with hysteresis so a half-held trigger does not chatter.
        bool triggerHeld(float value, bool wasHeld);
    }
}

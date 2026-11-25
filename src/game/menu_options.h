#pragma once

#include "core/settings.h"

#include <optional>
#include <vector>

namespace dx::game {
    // The GamepadGlyphs values: 0 Xbox, 1 PlayStation and 2 Switch, as glyphs::Style, and Auto,
    // added after them so earlier settings keep their meaning.
    inline constexpr int autoButtonPrompts = 3;

    enum class Platform { Desktop,
        Switch };

    // A menu choice as a platform has it: whether the menus offer it, and the setting it starts
    // with, which is also the one always used when the choice is not offered.
    struct MenuChoice {
        bool offered = true;
        int value = 0;
    };

    // The options menus as a platform has them. The menu code adds and takes out choices by this
    // alone; a new platform difference in the menus is a new field here.
    struct MenuOptions {
        // Display: Full Screen and Screen Resolution.
        bool fullscreen = true;
        bool resolution = true;
        // Settings: the Keyboard/Mouse button, which sets up the key bindings.
        bool keyBindings = true;
        // Controls: Mouse Sensitivity.
        bool mouseSensitivity = true;
        // Controls: Button Prompts, the GamepadGlyphs setting (see autoButtonPrompts), with the
        // setting's desktop default.
        MenuChoice buttonPrompts { true, static_cast<int>(settings::defaultNumber(settings::gamepadGlyphs.setting, dx::platform::Kind::Desktop)) };
    };

    // The table: each platform's options, whichever platform the program is built for.
    // platform::menuOptions() gives the build's own.
    const MenuOptions& menuOptionsFor(Platform platform);

    // A choice the platform's menus take out: the screen class and the choice's class, both in
    // the DeusEx package.
    struct RemovedChoice {
        const char* screen;
        const char* choice;
    };

    // The choices to take out of the options screens, by screen in menu order.
    std::vector<RemovedChoice> removedChoices(const MenuOptions& options);
    // The rows the controller options add to the Controls screen.
    int controllerRows(const MenuOptions& options);
    // The button prompts to show, as glyphs::Style, for the GamepadGlyphs setting chosen:
    // the style named, or for Auto (and any value no menu writes) the one detected for the
    // pad in use.
    int buttonPromptStyle(const MenuOptions& options, int chosen, int detected);
}

#include "game/menu_options.h"

namespace dx::game {
    const MenuOptions& menuOptionsFor(Platform platform)
    {
        static const MenuOptions desktop {};
        // One screen, played with a controller and the touch screen: no window to size, no
        // keyboard or mouse to set up, and the console's own buttons on the prompts.
        static const MenuOptions nintendoSwitch {
            .fullscreen = false,
            .resolution = false,
            .keyBindings = false,
            .mouseSensitivity = false,
            .buttonPrompts = { .offered = false, .value = static_cast<int>(settings::defaultNumber(settings::gamepadGlyphs.setting, dx::platform::Kind::Switch)) },
        };
        switch (platform) {
        case Platform::Desktop:
            return desktop;
        case Platform::Switch:
            return nintendoSwitch;
        }
        return desktop;
    }

    std::vector<RemovedChoice> removedChoices(const MenuOptions& options)
    {
        // The menus always draw over the 3D view, so there is no UI/Menu Background to pick.
        std::vector<RemovedChoice> removed { { "MenuScreenOptions", "MenuChoice_UIBackground" } };
        if (!options.fullscreen) {
            removed.push_back({ "MenuScreenDisplay", "MenuChoice_FullScreen" });
        }
        if (!options.resolution) {
            removed.push_back({ "MenuScreenDisplay", "MenuChoice_Resolution" });
        }
        // The pad looks through its own axes, which the mouse's sensitivity does not touch.
        if (!options.mouseSensitivity) {
            removed.push_back({ "MenuScreenControls", "MenuChoice_MouseSensitivity" });
        }
        return removed;
    }

    // Look Speed, Aim Assist, Gyro Aiming, Gyro Space, Gyro Sensitivity and Flick Stick, and
    // Button Prompts where it is offered.
    int controllerRows(const MenuOptions& options)
    {
        return 6 + (options.buttonPrompts.offered ? 1 : 0);
    }

    int buttonPromptStyle(const MenuOptions& options, int chosen, int detected)
    {
        int setting = options.buttonPrompts.offered ? chosen : options.buttonPrompts.value;
        return setting >= 0 && setting < autoButtonPrompts ? setting : detected;
    }
}

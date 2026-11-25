#include "game/menu_options.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

using dx::game::menuOptionsFor;
using dx::game::Platform;

namespace {
    std::vector<std::string> removedNames(Platform platform)
    {
        std::vector<std::string> names;
        for (const dx::game::RemovedChoice& removed : dx::game::removedChoices(menuOptionsFor(platform))) {
            names.push_back(std::string(removed.screen) + "." + removed.choice);
        }
        return names;
    }
}

TEST_CASE("the desktop menus offer every option", "[menu_options]")
{
    const dx::game::MenuOptions& desktop = menuOptionsFor(Platform::Desktop);
    CHECK(desktop.fullscreen);
    CHECK(desktop.resolution);
    CHECK(desktop.keyBindings);
    CHECK(desktop.mouseSensitivity);
    CHECK(desktop.buttonPrompts.offered);
    CHECK(desktop.buttonPrompts.value == dx::game::autoButtonPrompts);
    CHECK(removedNames(Platform::Desktop) == std::vector<std::string> { "MenuScreenOptions.MenuChoice_UIBackground" });
    CHECK(dx::game::controllerRows(desktop) == 7);
}

TEST_CASE("the Switch menus have no window, keyboard or mouse options", "[menu_options]")
{
    const dx::game::MenuOptions& console = menuOptionsFor(Platform::Switch);
    CHECK_FALSE(console.fullscreen);
    CHECK_FALSE(console.resolution);
    CHECK_FALSE(console.keyBindings);
    CHECK_FALSE(console.mouseSensitivity);
    CHECK_FALSE(console.buttonPrompts.offered);
    CHECK(removedNames(Platform::Switch) == std::vector<std::string> { "MenuScreenOptions.MenuChoice_UIBackground", "MenuScreenDisplay.MenuChoice_FullScreen", "MenuScreenDisplay.MenuChoice_Resolution", "MenuScreenControls.MenuChoice_MouseSensitivity" });
    // Look Speed, Aim Assist, the three gyro rows and Flick Stick, without Button Prompts.
    CHECK(dx::game::controllerRows(console) == 6);
}

TEST_CASE("button prompts follow the setting only where the choice is offered", "[menu_options]")
{
    const dx::game::MenuOptions& desktop = menuOptionsFor(Platform::Desktop);
    // A style chosen overrides the pad's own, whatever pad is in use.
    for (int detected : { 0, 1, 2 }) {
        CHECK(dx::game::buttonPromptStyle(desktop, 0, detected) == 0);
        CHECK(dx::game::buttonPromptStyle(desktop, 1, detected) == 1);
        CHECK(dx::game::buttonPromptStyle(desktop, 2, detected) == 2);
        // Auto, and values no menu writes, follow the pad.
        CHECK(dx::game::buttonPromptStyle(desktop, dx::game::autoButtonPrompts, detected) == detected);
        CHECK(dx::game::buttonPromptStyle(desktop, -1, detected) == detected);
        CHECK(dx::game::buttonPromptStyle(desktop, 7, detected) == detected);
    }
    // The Switch always shows its own buttons, whatever the ini says or the pad is.
    const dx::game::MenuOptions& console = menuOptionsFor(Platform::Switch);
    CHECK(console.buttonPrompts.value == 2);
    for (int chosen : { 0, 1, 2, 3, 5 }) {
        for (int detected : { 0, 1, 2 }) {
            CHECK(dx::game::buttonPromptStyle(console, chosen, detected) == 2);
        }
    }
}

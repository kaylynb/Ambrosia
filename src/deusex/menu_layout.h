#pragma once

#include "core/object.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dx::deusex {
    // A cut-out in a menu's background art that a widget sits in, in client-area pixels: the
    // widget's own rectangle and the art's outline round it.
    struct MenuSlot {
        // A solid slot (an action button, or a slider's track, cut at all four corners), the
        // outline round a value field, and the thin outline round the help text.
        enum class Kind { Button,
            Field,
            TextBox,
            Track };
        Kind kind = Kind::Button;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    // The slots of an options screen's background art, as its defaults lay the screen out.
    struct MenuLayout {
        std::string screen;
        int width = 0;
        int height = 0;
        std::vector<MenuSlot> slots;
        // Optional, width by height: where the screen draws over the backdrop itself (text, lines,
        // a picture), which the traces keep clear of without anything being cut there.
        std::vector<uint8_t> drawn;
    };

    // The screens whose art can be replaced by a generated backdrop: the options screens.
    const std::vector<std::string>& generatedArtScreens();

    // An options screen's (a MenuUIScreenWindow's) layout from its defaults, the way its script
    // places the choices (MenuUIScreenWindow.CreateChoices) and help text, or nothing when the
    // screen or a property it needs is not there.
    std::optional<MenuLayout> optionsLayout(UClass* screen);
    // A short name for a layout that changes whenever a slot or the size does: the generated art
    // made for it is found by it.
    std::string layoutHash(const MenuLayout& layout);
    // The layout as JSON, with its hash.
    std::string layoutJson(const MenuLayout& layout);

    // Writes <Screen>.json (its layoutJson) and <Screen>.png, its generated backdrop, into
    // `directory` for each of generatedArtScreens, as the screens are now (after the startup
    // changes to the menus).
    void dumpMenuLayouts(const std::filesystem::path& directory);
    // Gives each of generatedArtScreens the backdrop generated for its layout now
    // (generateBackdrop, with backdropSeed), in place of its spliced art. Runs after every other
    // startup change to the menus.
    void applyGeneratedMenuArt();
}

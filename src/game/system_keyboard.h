#pragma once

#include "deusex/text_entry.h"

#include <optional>
#include <string>

namespace dx::game::systemkeyboard {
    // Whether text is typed on a system keyboard rather than a real one: on the Switch.
    bool available();
    // Shows it for a text field and returns what was typed (UTF-8), or nothing when the player
    // cancels or there is no system keyboard. Returns once the keyboard has closed.
    std::optional<std::string> show(const textentry::Request& request);
    // Opens it for a text field and puts what was typed there. False when the window is no
    // text field the player can type in, or there is no system keyboard.
    bool edit(UObject* window);
    // The same for the focused window, leaving the press to it when it is no text field.
    bool editFocused();
}

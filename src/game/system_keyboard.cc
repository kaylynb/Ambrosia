#include "game/system_keyboard.h"

#include "extension/extension.h"

namespace dx::game::systemkeyboard {
    bool editFocused()
    {
        return edit(extension::focusWindow());
    }

    bool edit(UObject* window)
    {
        std::optional<textentry::Request> request = available() ? textentry::requestFor(window) : std::nullopt;
        if (!request) {
            return false;
        }
        if (std::optional<std::string> typed = show(*request)) {
            textentry::deliver(*request, *typed);
        }
        return true;
    }
}

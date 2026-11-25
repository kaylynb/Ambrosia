#include "game/system_keyboard.h"

// Text is typed on a real keyboard, through SDL's text input events.
namespace dx::game::systemkeyboard {
    bool available()
    {
        return false;
    }

    std::optional<std::string> show(const textentry::Request&)
    {
        return std::nullopt;
    }
}

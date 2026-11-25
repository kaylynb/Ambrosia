#include "game/system_keyboard.h"

#include <switch.h>

#include <algorithm>
#include <cstring>

// The keyboard applet, the only way to type without a keyboard plugged in.
namespace dx::game::systemkeyboard {
    bool available()
    {
        return true;
    }

    std::optional<std::string> show(const textentry::Request& request)
    {
        // The keyboard applet takes at most 500 characters.
        uint32_t limit = static_cast<uint32_t>(request.maxLength > 0 ? std::min(request.maxLength, 500) : 500);
        // Four bytes are enough for any character the game can show. Made before the applet, so
        // nothing that can throw comes between swkbdCreate and swkbdClose.
        std::string typed(static_cast<size_t>(limit) * 4 + 1, '\0');
        SwkbdConfig keyboard;
        if (R_FAILED(swkbdCreate(&keyboard, 0))) {
            return std::nullopt;
        }
        if (request.password) {
            swkbdConfigMakePresetPassword(&keyboard);
        } else {
            swkbdConfigMakePresetDefault(&keyboard);
        }
        swkbdConfigSetReturnButtonFlag(&keyboard, request.multiline ? 1 : 0);
        swkbdConfigSetStringLenMax(&keyboard, limit);
        swkbdConfigSetInitialText(&keyboard, request.text.c_str());
        Result result = swkbdShow(&keyboard, typed.data(), typed.size());
        swkbdClose(&keyboard);
        // Cancelling is a failed result too.
        if (R_FAILED(result)) {
            return std::nullopt;
        }
        typed.resize(std::strlen(typed.c_str()));
        return typed;
    }
}

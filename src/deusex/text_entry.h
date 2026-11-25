#pragma once

#include "core/objects.h"

#include <cstdint>
#include <optional>
#include <string>

namespace dx::textentry {
    // What a system keyboard needs to edit one of the game's text fields: what the field holds,
    // as UTF-8, how long it may grow, and how to show it.
    struct Request {
        UObject* field = nullptr;
        std::string text;
        int32_t maxLength = 0;
        // A computer's password or an ATM's PIN.
        bool password = false;
        bool multiline = false;
    };

    // The window, when it is a text field the player can type in now.
    std::optional<Request> requestFor(UObject* window);
    // Puts what the player typed (UTF-8) in the field as typing it would, through the field's
    // filter and length limit, replacing what was there. A one-line field then takes Enter, as
    // a keyboard player ends the entry: a save is made, a login tried, the next field focused.
    void deliver(const Request& request, const std::string& typed);
}

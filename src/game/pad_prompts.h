#pragma once

#include "deusex/glyphs.h"

#include <SDL3/SDL_gamepad.h>

#include <optional>
#include <vector>

namespace dx::game {
    // The prompts that match a pad's printed buttons, from what SDL says it is: its type, its
    // type before any mapping override, and the label SDL gives its bottom face button. Pads
    // SDL cannot name fall back on that label, so a third-party pad mapped with Nintendo or
    // Sony faces still gets their glyphs; anything else is drawn as an Xbox pad.
    glyphs::Style promptStyleFor(SDL_GamepadType type, SDL_GamepadType realType, SDL_GamepadButtonLabel southLabel);

    // The connected pads in the order they were last used, which decides the prompts in the
    // Auto setting: the pad that last pressed a button or pushed a stick past the used
    // threshold, or the one connected most recently. Removing that pad hands the prompts to
    // the one used before it.
    class PromptPads {
    public:
        void connected(SDL_JoystickID id, glyphs::Style style);
        void removed(SDL_JoystickID id);
        void used(SDL_JoystickID id);
        // The style of the pad in use, or none with no pad connected.
        std::optional<glyphs::Style> style() const;
        // The pad in use, or none with no pad connected.
        std::optional<SDL_JoystickID> current() const;

    private:
        struct Pad {
            SDL_JoystickID id;
            glyphs::Style style;
        };

        // The most recently used first.
        std::vector<Pad> pads;
    };
}

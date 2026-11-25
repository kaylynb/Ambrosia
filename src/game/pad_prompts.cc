#include "game/pad_prompts.h"

#include <algorithm>

namespace dx::game {
    namespace {
        // Compared rather than switched on, as the types SDL has differ between its versions
        // (the Switch's SDL has no Steam Controller type).
        std::optional<glyphs::Style> styleOfType(SDL_GamepadType type)
        {
            if (type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE) {
                return glyphs::Style::Xbox;
            }
            if (type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5) {
                return glyphs::Style::PlayStation;
            }
            if (type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) {
                return glyphs::Style::Switch;
            }
            // The rest: the GameCube pad, with its A at the bottom as an Xbox pad has it, the
            // Steam Controller, laid out as an Xbox pad, and pads SDL cannot name.
            return std::nullopt;
        }
    }

    glyphs::Style promptStyleFor(SDL_GamepadType type, SDL_GamepadType realType, SDL_GamepadButtonLabel southLabel)
    {
        // The type as overridden first: Steam and the SDL_GAMECONTROLLERTYPE hint use the
        // override to say what a pad really is.
        if (auto style = styleOfType(type)) {
            return *style;
        }
        if (auto style = styleOfType(realType)) {
            return *style;
        }
        if (southLabel == SDL_GAMEPAD_BUTTON_LABEL_CROSS) {
            return glyphs::Style::PlayStation;
        }
        if (southLabel == SDL_GAMEPAD_BUTTON_LABEL_B) {
            return glyphs::Style::Switch;
        }
        return glyphs::Style::Xbox;
    }

    void PromptPads::connected(SDL_JoystickID id, glyphs::Style style)
    {
        std::erase_if(pads, [id](const Pad& pad) { return pad.id == id; });
        pads.insert(pads.begin(), { id, style });
    }

    void PromptPads::removed(SDL_JoystickID id)
    {
        std::erase_if(pads, [id](const Pad& pad) { return pad.id == id; });
    }

    void PromptPads::used(SDL_JoystickID id)
    {
        auto found = std::find_if(pads.begin(), pads.end(), [id](const Pad& pad) { return pad.id == id; });
        if (found != pads.end()) {
            std::rotate(pads.begin(), found, found + 1);
        }
    }

    std::optional<glyphs::Style> PromptPads::style() const
    {
        if (pads.empty()) {
            return std::nullopt;
        }
        return pads.front().style;
    }

    std::optional<SDL_JoystickID> PromptPads::current() const
    {
        if (pads.empty()) {
            return std::nullopt;
        }
        return pads.front().id;
    }
}

#pragma once

#include "core/object.h"
#include "deusex/prompts.h"
#include "engine/gamepad.h"

#include <vector>

namespace dx::gamepadmenu {
    // Gamepad shortcuts for Deus Ex's own screens: PreviousTab and NextTab page through the
    // persona screens and other actions press a screen's buttons, such as Equip and Drop in
    // the inventory. Which button each uses is in the mapping, engine/gamepad.h. The inventory (pad_inventory.h) and the computers and keypads
    // (terminals.h) have their own on top. A prompt bar shows what each button does.

    // Handles a press while a menu is open. Returns false when the screen has nothing on that
    // button, so the caller falls back to the generic menu keys.
    bool press(gamepad::Button button);
    // What each button does on the screen on top.
    std::vector<prompts::Prompt> prompts();
    // Left and right on an options row change its value, as the keyboard's arrows do, but a
    // slider stops at its ends rather than wrapping round. Returns false for any other key or
    // when focus is not on an options row, leaving the key to move focus.
    bool adjust(uint8_t key);
    // Whether gamepad focus should pass over a window: the persona tab bar is left to the
    // bumpers so the d-pad stays within the screen, and a slider's own scale to its row, so
    // up and down always leave it.
    bool skipForNavigation(UObject* window);
    // Whether a control shows gamepad focus itself and needs no outline: a conversation's
    // reply lights up in its focus color.
    bool ownFocusLook(UObject* window);
}

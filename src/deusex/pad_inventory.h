#pragma once

#include "core/object.h"
#include "deusex/prompts.h"
#include "engine/canvas.h"
#include "engine/gamepad.h"

#include <cstdint>
#include <vector>

namespace dx::padinventory {
    // The inventory screen with a pad. X picks the focused item up to move it over the grid
    // and the belt; A or X places it. Otherwise A uses the selected item and the right stick
    // sorts the grid. Y equips/unequips and LT drops the focused item, selecting it first.
    // Applying a weapon mod, which uses it up, is asked about first, whether the pad or the
    // mouse dropped it on the weapon.

    // Handles a press on the inventory screen, or anywhere while an item is held or the mod
    // question is open. Returns whether it used the button.
    bool press(UObject* top, gamepad::Button button);
    // Whether an item is held or the mod question is open, when prompts() replaces the screen's.
    bool busy();
    std::vector<prompts::Prompt> busyPrompts();
    // The inventory's own prompts for the focused control, added to the screen's.
    void addPrompts(UObject* top, std::vector<prompts::Prompt>& out);
    // Whether A on the focused item uses it rather than selecting it.
    bool usesOnA(UObject* top);
    // Whether focus is on this inventory screen's quickbar rather than the item grid.
    bool beltFocused(UObject* top);
    // Enables quickbar focus when browsing; while an item is held, steers it and returns true.
    bool steer(uint8_t arrowKey);

    // The extension's button hook: the mod question answered with the mouse.
    bool buttonPressed(UObject* button);
    // The mod question answered from the keyboard: Enter, Space or Y for yes, Escape or N for no.
    bool keyPressed(uint8_t key);
    // The extension's mouse release hook: a mouse drag of a weapon mod onto a weapon it fits
    // is held back and asked about in the same way.
    bool mouseReleased(UObject* window, uint8_t button);

    // Shows where a held item will land.
    void drawOverlay(Canvas2D& canvas);
}

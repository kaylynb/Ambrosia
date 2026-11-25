#pragma once

#include "core/object.h"
#include "engine/canvas.h"
#include "engine/gamepad.h"

#include <string>
#include <vector>

namespace dx::prompts {
    // What a button does, for the bar of prompts under a menu.
    // The glyphs come from the action's mapping, with its alternative, if any, drawn as a
    // compact "A / X" group.
    struct Prompt {
        gamepad::Action action;
        // Empty for a glyph that shares the next prompt's label, as the bumpers share "Tabs".
        std::string label;
        // A keyboard key spelt out in place of a glyph, such as "F1".
        std::string key = {};
    };

    // The prompts in a row under the top modal window's content, in its theme.
    void drawBar(Canvas2D& canvas, const std::vector<Prompt>& prompts);
    // A line of text on the prompts' panel at the top of the screen, such as that the
    // controller was disconnected.
    void drawNotice(Canvas2D& canvas, const std::string& text);
    // What Interact does to the object under the crosshair, such as "Pick up".
    void drawInteraction(Canvas2D& canvas, UObject* player);
    // That label, as the game's own frob decides: "Pick up", "Search", "Talk" or "Use".
    const char* interactionLabel(UObject* target, UObject* player);
}

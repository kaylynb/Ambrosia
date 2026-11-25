#pragma once

#include "engine/canvas.h"
#include "engine/gamepad.h"

#include <string_view>

namespace dx {
    class UTexture;
}

namespace dx::glyphs {
    // The embedded controller button art, Kenney's Input Prompts: white, to be tinted with a
    // theme color, 128 pixels square and drawn smoothly at any size.

    // Which controller's buttons are shown. Every layout is positional: A is the bottom face
    // button, drawn as Cross on a PlayStation pad and B on a Switch pad.
    enum class Style { Xbox,
        PlayStation,
        Switch };
    void setStyle(Style style);
    Style style();
    // The style that matches the pad in use, which the Auto setting shows. Kept apart from
    // style() so the layout screen can preview Auto before the setting is saved.
    void setPadStyle(Style style);
    Style padStyle();

    // A glyph by its Xbox file name without extension, such as "pad_a" or "pad_dpad_up", in
    // the current style. Buttons and actions should use the overloads below, which take their
    // names from the mapping in engine/gamepad.h; this one is for other art, like the layout.
    UTexture* image(std::string_view name);
    // A button's own glyph, a single direction for the d-pad.
    UTexture* image(gamepad::Button button);
    // A control's glyph: the button's own, or the stick moved, or the whole d-pad.
    UTexture* image(gamepad::Control control);
    // The glyph of an action's primary control.
    UTexture* image(gamepad::Action action);

    // A glyph size near wanted: whole pixels, and no smaller than 16.
    float size(float wanted);
    void draw(Canvas2D& canvas, UTexture* image, float x, float y, float size, LinearColor color);
}

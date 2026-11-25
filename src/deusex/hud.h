#pragma once

#include "core/object.h"

namespace dx::hud {
    // The player the root window belongs to, whose color theme overlays use.
    UObject* viewer();
    // Whether the game's HUD is up. It is hidden on the title map, in the intro and in
    // cutscenes, where the pad's own overlays and play controls keep away too.
    bool shown();
    // Whether this is the title map behind the main menu, where saving makes no sense.
    bool atTitleMap();
    // With a pad, hides the HUD's object belt and augmentation bar, whose jobs the radials and
    // the d-pad quick slots take over; without one, restores the player's settings.
    void layout(bool gamepadMode);
}

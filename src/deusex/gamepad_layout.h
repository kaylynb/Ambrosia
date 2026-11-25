#pragma once

#include "engine/canvas.h"

namespace dx {
    class UObject;
}

namespace dx::padlayout {
    // The Gamepad screen's Controller Layout and Calibrate Gyro rows: choices (MenuUIChoiceAction
    // subclasses, in DeusEx) whose buttons do nothing of their own; buttonPressed and the game's
    // button hook (isCalibrateButton) take them.
    inline constexpr const char* layoutChoice = "MenuChoice_ControllerLayout";
    inline constexpr const char* calibrateChoice = "MenuChoice_CalibrateGyro";

    // Makes the read-only gameplay diagram's screen and the two rows above.
    void install();
    // Opens the diagram, for the Controller Layout row's button.
    bool buttonPressed(UObject* button);
    // Draws the diagram when the layout screen is on top, for keyboard and pad users alike, and
    // says whether it is. Drawn as the extension's overlay (setOverlayDrawer), so the cursor stays
    // over it.
    bool draw(Canvas2D& canvas);
    // Whether the layout screen is on top.
    bool shown();
    // Whether the pad in play has back paddles, which the diagram then lists along its bottom.
    void showPaddles(bool shown);
    // The Calibrate Gyro row's button, which the game runs (GamepadInput).
    bool isCalibrateButton(UObject* button);
    // Sets that button's label on the open screen, if it has the row.
    void setCalibrateLabel(const char* label);
    inline constexpr const char* calibrateLabel = "Calibrate |&Gyro";
}

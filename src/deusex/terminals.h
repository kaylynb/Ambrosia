#pragma once

#include "core/object.h"
#include "deusex/prompts.h"
#include "engine/gamepad.h"

#include <cstdint>
#include <vector>

namespace dx::terminals {
    // Computers, ATMs and keypads, kept off the keyboard where the game allows. With a pad, Y
    // presses the Ice Breaker's Hack button and X opens a picker of known credentials,
    // logging in after choosing one, or dials a known door code.
    // Mouse users have matching Autologin / Enter <code> buttons on the screens.

    // Handles a press on a terminal or keypad. Returns whether it used the button.
    bool press(UObject* top, gamepad::Button button);
    void addPrompts(UObject* top, std::vector<prompts::Prompt>& out);
    // The native terminal action buttons and credential picker.
    bool buttonPressed(UObject* button);
    // Each frame: updates the terminal button, dials a door code key by key, and fills an
    // ATM's balance into the amount to withdraw with Withdraw focused.
    void tick(float deltaSeconds);
}

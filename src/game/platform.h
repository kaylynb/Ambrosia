#pragma once

#include "game/menu_options.h"
#include "game/touch_cursor.h"

#include <SDL3/SDL_video.h>

#include <optional>

// What differs between the platforms the game runs on. Each platform's definitions are in
// game/desktop or game/switch, of which the build compiles one; the system keyboard, the gyro
// and the start-up report (system_keyboard.h, switch_gyro.h, console_report.h) are split the
// same way.
namespace dx::game::platform {
    // The steps before the options are read. On the Switch: sends standard output and errors to
    // the log, reads the options from a file (replacing argc and argv, whose strings then last
    // until the program ends), refuses to run from the album and checks for the game data. An
    // exit code when the program must stop, having said why.
    std::optional<int> start(int& argc, char**& argv);
    // Says where to look when the options are not valid, after parseOptions has said what is
    // wrong on standard error.
    void reportInvalidOptions();
    // Starts SDL's text input events for the window, where they come from a real keyboard.
    // Where starting them opens an on-screen keyboard (onScreenKeyboard), they are left off
    // until a text field is edited.
    void startTextInput(SDL_Window* window);
    // Whether SDL shows an on-screen keyboard while text input is on, as it says
    // (SDL_HasScreenKeyboardSupport): Steam's on a Steam Deck, for one.
    bool onScreenKeyboard();
    // Where onScreenKeyboard: turns text input on for a field at area (window coordinates),
    // which opens the keyboard beside it, or off with null. Elsewhere text input is kept on for
    // the real keyboard, whatever is asked. Does nothing on the Switch.
    void editTextAt(SDL_Window* window, const SDL_Rect* area);
    // Puts the window into fullscreen, exclusive at the closest display mode to width by height
    // when exclusive is set and borderless otherwise, or into a window of width by height.
    // False when the platform's window is fixed and is left as it is.
    bool setVideoMode(SDL_Window* window, bool fullscreen, bool exclusive, int width, int height);
    // Gives the touch cursor this frame's fingers on the touch screen, where the platform has
    // one that drives the cursor (the Switch). Does nothing elsewhere.
    void readTouch(TouchCursor& touch);
    // Removes the GL driver's on-disk shader cache, for measuring a start without it (the newer
    // Mesa on the Switch keeps one in sdmc:/.mesa). Says on standard error what it did.
    void clearShaderCache();
    // Which menu options this platform has (menu_options.h).
    const MenuOptions& menuOptions();
}

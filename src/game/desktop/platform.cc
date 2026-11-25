#include "game/platform.h"

#include "core/log.h"
#include "game/console_report.h"

#include <SDL3/SDL_keyboard.h>

namespace dx::game::platform {
    std::optional<int> start(int& argc, char**& argv)
    {
        // The log file, and a console with --console (console_report).
        report::start(log::consoleRequested(argc, argv));
        return std::nullopt;
    }

    void reportInvalidOptions()
    {
    }

    void startTextInput(SDL_Window* window)
    {
        if (!onScreenKeyboard()) {
            SDL_StartTextInput(window);
        }
    }

    // SDL says whether a keyboard shows on screen while text input is on: Steam's on a Steam
    // Deck, the system's under Wayland with no physical keyboard, and so on. Asked each time,
    // as a keyboard can be plugged in or out.
    bool onScreenKeyboard()
    {
        return SDL_HasScreenKeyboardSupport();
    }

    void editTextAt(SDL_Window* window, const SDL_Rect* area)
    {
        // Without one, text input stays on for the real keyboard.
        if (!onScreenKeyboard()) {
            if (!SDL_TextInputActive(window)) {
                SDL_StartTextInput(window);
            }
            return;
        }
        if (area) {
            SDL_SetTextInputArea(window, area, 0);
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }

    bool setVideoMode(SDL_Window* window, bool fullscreen, bool exclusive, int width, int height)
    {
        if (fullscreen) {
            SDL_DisplayMode mode {};
            if (exclusive && SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(window), width, height, 0.0f, false, &mode)) {
                SDL_SetWindowFullscreenMode(window, &mode);
            } else {
                SDL_SetWindowFullscreenMode(window, nullptr);
            }
            SDL_SetWindowFullscreen(window, true);
        } else {
            SDL_SetWindowFullscreen(window, false);
            SDL_SetWindowSize(window, width, height);
        }
        return true;
    }

    // A desktop's touch screen, if it has one, acts as its mouse.
    void readTouch(TouchCursor&)
    {
    }

    void clearShaderCache()
    {
        // Desktop drivers keep their caches where the game cannot clear them.
        log::info("Init", "shader cache: none the game can clear on this platform");
    }

    const MenuOptions& menuOptions()
    {
        return menuOptionsFor(Platform::Desktop);
    }
}

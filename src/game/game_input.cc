#include "game/game.h"

#include "core/sdl_handles.h"
#include "core/vm.h"
#include "deusex/pad_inventory.h"
#include "deusex/text_entry.h"
#include "deusex/text_input.h"
#include "engine/console.h"
#include "engine/input.h"
#include "extension/extension.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>

namespace dx::game {
    namespace {
        uint8_t translateKey(SDL_Keycode key)
        {
            if (key >= SDLK_A && key <= SDLK_Z) {
                return static_cast<uint8_t>(dx::IK_A + (key - SDLK_A));
            }
            if (key >= SDLK_0 && key <= SDLK_9) {
                return static_cast<uint8_t>(dx::IK_0 + (key - SDLK_0));
            }
            if (key >= SDLK_F1 && key <= SDLK_F12) {
                return static_cast<uint8_t>(dx::IK_F1 + (key - SDLK_F1));
            }
            if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
                return static_cast<uint8_t>(dx::IK_NumPad0 + 1 + (key - SDLK_KP_1));
            }
            switch (key) {
            case SDLK_KP_0:
                return dx::IK_NumPad0;
            case SDLK_BACKSPACE:
                return dx::IK_Backspace;
            case SDLK_TAB:
                return dx::IK_Tab;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                return dx::IK_Enter;
            case SDLK_LSHIFT:
            case SDLK_RSHIFT:
                return dx::IK_Shift;
            case SDLK_LCTRL:
            case SDLK_RCTRL:
                return dx::IK_Ctrl;
            case SDLK_LALT:
            case SDLK_RALT:
                return dx::IK_Alt;
            case SDLK_PAUSE:
                return dx::IK_Pause;
            case SDLK_CAPSLOCK:
                return dx::IK_CapsLock;
            case SDLK_ESCAPE:
                return dx::IK_Escape;
            case SDLK_SPACE:
                return dx::IK_Space;
            case SDLK_PAGEUP:
                return dx::IK_PageUp;
            case SDLK_PAGEDOWN:
                return dx::IK_PageDown;
            case SDLK_END:
                return dx::IK_End;
            case SDLK_HOME:
                return dx::IK_Home;
            case SDLK_LEFT:
                return dx::IK_Left;
            case SDLK_UP:
                return dx::IK_Up;
            case SDLK_RIGHT:
                return dx::IK_Right;
            case SDLK_DOWN:
                return dx::IK_Down;
            case SDLK_INSERT:
                return dx::IK_Insert;
            case SDLK_DELETE:
                return dx::IK_Delete;
            case SDLK_KP_MULTIPLY:
                return dx::IK_GreyStar;
            case SDLK_KP_PLUS:
                return dx::IK_GreyPlus;
            case SDLK_KP_MINUS:
                return dx::IK_GreyMinus;
            case SDLK_KP_PERIOD:
                return dx::IK_NumPadPeriod;
            case SDLK_KP_DIVIDE:
                return dx::IK_GreySlash;
            case SDLK_NUMLOCKCLEAR:
                return dx::IK_NumLock;
            case SDLK_SCROLLLOCK:
                return dx::IK_ScrollLock;
            case SDLK_SEMICOLON:
                return dx::IK_Semicolon;
            case SDLK_EQUALS:
                return dx::IK_Equals;
            case SDLK_COMMA:
                return dx::IK_Comma;
            case SDLK_MINUS:
                return dx::IK_Minus;
            case SDLK_PERIOD:
                return dx::IK_Period;
            case SDLK_SLASH:
                return dx::IK_Slash;
            case SDLK_GRAVE:
                return dx::IK_Tilde;
            case SDLK_LEFTBRACKET:
                return dx::IK_LeftBracket;
            case SDLK_BACKSLASH:
                return dx::IK_Backslash;
            case SDLK_RIGHTBRACKET:
                return dx::IK_RightBracket;
            case SDLK_APOSTROPHE:
                return dx::IK_SingleQuote;
            default:
                return dx::IK_None;
            }
        }

        uint8_t translateButton(uint8_t button)
        {
            switch (button) {
            case SDL_BUTTON_LEFT:
                return dx::IK_LeftMouse;
            case SDL_BUTTON_RIGHT:
                return dx::IK_RightMouse;
            case SDL_BUTTON_MIDDLE:
                return dx::IK_MiddleMouse;
            default:
                return dx::IK_None;
            }
        }
    }

    bool Game::consoleKeyEvent(uint8_t key, uint8_t action, float delta)
    {
        dx::UObject* console = scriptConsole();
        if (!console) {
            return false;
        }
        dx::vm::Call call(console, dx::Name("KeyEvent"));
        if (!call.valid()) {
            return false;
        }
        call.byte(key).byte(action).f(delta).run();
        return call.returnValue() && *reinterpret_cast<uint32_t*>(call.returnValue());
    }

    bool Game::consoleTyping()
    {
        dx::UObject* console = scriptConsole();
        // A bool shares its word with the console's other flags, so it is read by its property.
        dx::UProperty* typing = console ? console->cls->findProperty(dx::Name("bTyping")) : nullptr;
        return typing && typing->toText(console->data + typing->offset) == "True";
    }

    void Game::pasteIntoConsole()
    {
        for (char c : dx::textinput::consolePaste(dx::sdl::clipboardText())) {
            charEvent(c);
        }
    }

    void Game::charEvent(char c)
    {
        dx::UObject* console = scriptConsole();
        if (console) {
            dx::vm::Call call(console, dx::Name("KeyType"));
            if (call.valid()) {
                call.byte(static_cast<uint8_t>(c)).run();
                if (call.returnValue() && *reinterpret_cast<uint32_t*>(call.returnValue())) {
                    return;
                }
            }
        }
        dx::extension::charEvent(c);
    }

    void Game::handleKey(uint8_t key, bool pressed)
    {
        if (key == dx::IK_None) {
            return;
        }
        if (pressed && dx::padinventory::keyPressed(key)) {
            return;
        }
        uint8_t action = pressed ? dx::IST_Press : dx::IST_Release;
        if (consoleKeyEvent(key, action, 0)) {
            return;
        }
        if (dx::extension::keyEvent(key, action, 0)) {
            if (!pressed) {
                dx::console::key(key, action, 0);
            }
            return;
        }
        dx::console::key(key, action, 0);
    }

    void Game::processEvents()
    {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (overlay->event(event)) {
                continue;
            }
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                windowResized(event.window.data1, event.window.data2);
                break;
            case SDL_EVENT_KEY_DOWN:
                pad.keyboardUsed();
                if (event.key.key == SDLK_V && (event.key.mod & SDL_KMOD_CTRL) && consoleTyping()) {
                    // Ctrl+V on the console's line (the console, or Say) pastes the clipboard.
                    pasteIntoConsole();
                } else if (!event.key.repeat) {
                    handleKey(translateKey(event.key.key), true);
                } else {
                    repeatKey(translateKey(event.key.key));
                }
                break;
            case SDL_EVENT_KEY_UP:
                handleKey(translateKey(event.key.key), false);
                break;
            case SDL_EVENT_TEXT_INPUT:
                for (char c : dx::textinput::fromUtf8(event.text.text)) {
                    charEvent(c);
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (dx::extension::wantsMouse()) {
                    int width = 0;
                    int height = 0;
                    SDL_GetWindowSize(window, &width, &height);
                    int drawWidth = 0;
                    int drawHeight = 0;
                    SDL_GetWindowSizeInPixels(window, &drawWidth, &drawHeight);
                    float scaleX = width > 0 ? static_cast<float>(drawWidth) / static_cast<float>(width) : 1.0f;
                    float scaleY = height > 0 ? static_cast<float>(drawHeight) / static_cast<float>(height) : 1.0f;
                    dx::extension::mouseMove(event.motion.x * scaleX, event.motion.y * scaleY);
                } else {
                    dx::console::key(dx::IK_MouseX, dx::IST_Axis, event.motion.xrel);
                    dx::console::key(dx::IK_MouseY, dx::IST_Axis, -event.motion.yrel);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                pad.keyboardUsed();
                uint8_t key = translateButton(event.button.button);
                bool pressed = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                if (dx::extension::wantsMouse() && dx::extension::mouseButton(key, pressed)) {
                    // A click or a tap on a text field opens the on-screen keyboard, as A does.
                    if (!pressed && platform::onScreenKeyboard()) {
                        editOnScreen();
                    }
                    break;
                }
                handleKey(key, pressed);
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL: {
                uint8_t key = event.wheel.integer_y > 0 ? dx::IK_MouseWheelUp : dx::IK_MouseWheelDown;
                if (event.wheel.integer_y != 0) {
                    dx::console::key(dx::IK_MouseW, dx::IST_Axis, static_cast<float>(event.wheel.integer_y * 120));
                    handleKey(key, true);
                    handleKey(key, false);
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
                pad.handleEvent(event);
                break;
            default:
                break;
            }
        }
    }

    void Game::repeatKey(uint8_t key)
    {
        if (!consoleKeyEvent(key, dx::IST_Hold, 0)) {
            dx::extension::keyEvent(key, dx::IST_Hold, 0);
        }
    }

    bool Game::editOnScreen()
    {
        dx::UObject* focus = dx::extension::focusWindow();
        if (!platform::onScreenKeyboard() || !dx::textentry::requestFor(focus)) {
            return false;
        }
        editingField = focus;
        textInputOn = false;
        updateTextInput();
        return true;
    }

    void Game::updateTextInput()
    {
        // With no keyboard on screen, text input stays on for the real one.
        if (!platform::onScreenKeyboard()) {
            editingField = nullptr;
            textInputOn = true;
            platform::editTextAt(window, nullptr);
            return;
        }
        dx::UObject* focus = dx::extension::focusWindow();
        if (editingField && (focus != editingField || !dx::textentry::requestFor(focus))) {
            editingField = nullptr;
        }
        bool overlayTyping = overlay && overlay->mode() == dx::debugui::Overlay::Mode::Interactive;
        bool wanted = editingField || consoleTyping() || overlayTyping;
        if (wanted == textInputOn) {
            return;
        }
        textInputOn = wanted;
        if (!wanted) {
            platform::editTextAt(window, nullptr);
            return;
        }
        // The keyboard goes beside the field; the console and overlay give no place.
        SDL_Rect area {};
        if (std::optional<dx::extension::Area> field = editingField ? dx::extension::windowArea(editingField) : std::nullopt) {
            int width = 0;
            int height = 0;
            int drawWidth = 0;
            int drawHeight = 0;
            SDL_GetWindowSize(window, &width, &height);
            SDL_GetWindowSizeInPixels(window, &drawWidth, &drawHeight);
            float toWindow = dx::extension::uiScale() * (drawWidth > 0 ? static_cast<float>(width) / static_cast<float>(drawWidth) : 1.0f);
            area = { static_cast<int>(field->x * toWindow), static_cast<int>(field->y * toWindow), static_cast<int>(field->width * toWindow), static_cast<int>(field->height * toWindow) };
        }
        platform::editTextAt(window, &area);
    }

    void Game::sendKey(uint8_t key, bool pressed)
    {
        if ((key == dx::IK_LeftMouse || key == dx::IK_RightMouse) && dx::extension::wantsMouse() && dx::extension::mouseButton(key, pressed)) {
            return;
        }
        handleKey(key, pressed);
    }
}

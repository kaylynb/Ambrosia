#include "extension/windows.h"

#include "extension/window_internal.h"
#include "extension/window_natives.h"

#include <cctype>

namespace dx::extension {
    void nativeSetAcceleratorText(UObject* object, Frame& stack, void*)
    {
        std::string text = vm::get<std::string>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        if (!window) {
            return;
        }
        int32_t key = 0;
        size_t pos = text.find("|&");
        if (pos != std::string::npos && pos + 2 < text.size()) {
            key = std::toupper(static_cast<unsigned char>(text[pos + 2]));
        }
        window::acceleratorKey(window) = key;
        if (UWindow* modal = window->modalWindow()) {
            modalwindow::bDirtyAccelerators(modal) = 1;
        }
    }

    void nativeSetFocusWindow(UObject* object, Frame& stack, void* result)
    {
        UObject* target = vm::get<UObject*>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        vm::retBool(result, window && window->setFocusWindow(windowOf(target)));
    }

    void nativeGetFocusWindow(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        URootWindow* rootWindow = window ? window->root() : nullptr;
        vm::ret<UObject*>(result, rootWindow ? rootWindow->focusWindow() : nullptr);
    }

    void nativeIsFocusWindow(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        URootWindow* rootWindow = window ? window->root() : nullptr;
        vm::retBool(result, rootWindow && rootWindow->focusWindow() == window);
    }

    void nativeMoveFocusLeft(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<UObject*>(result, window && window->root() ? window->root()->moveFocus(0) : nullptr);
    }

    void nativeMoveFocusRight(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<UObject*>(result, window && window->root() ? window->root()->moveFocus(1) : nullptr);
    }

    void nativeMoveFocusUp(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<UObject*>(result, window && window->root() ? window->root()->moveFocus(2) : nullptr);
    }

    void nativeMoveFocusDown(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<UObject*>(result, window && window->root() ? window->root()->moveFocus(3) : nullptr);
    }

    void nativeMoveTabGroupNext(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<UObject*>(result, window && window->root() ? window->root()->moveTabGroup(3) : nullptr);
    }

    void nativeMoveTabGroupPrev(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<UObject*>(result, window && window->root() ? window->root()->moveTabGroup(2) : nullptr);
    }

    namespace {
        void convertCoordinates(UWindow* from, float fromX, float fromY, UWindow* to, float& toX, float& toY)
        {
            float ax = 0;
            float ay = 0;
            if (from) {
                from->absolutePosition(ax, ay);
            }
            float bx = 0;
            float by = 0;
            if (to) {
                to->absolutePosition(bx, by);
            }
            toX = fromX + ax - bx;
            toY = fromY + ay - by;
        }
    }

    void nativeConvertCoordinates(UObject*, Frame& stack, void*)
    {
        UObject* from = vm::get<UObject*>(stack);
        float fromX = vm::get<float>(stack);
        float fromY = vm::get<float>(stack);
        UObject* to = vm::get<UObject*>(stack);
        float* toX = vm::getRef<float>(stack);
        float* toY = vm::getRef<float>(stack);
        vm::finish(stack);
        convertCoordinates(windowOf(from), fromX, fromY, windowOf(to), *toX, *toY);
    }

    void nativeGrabMouse(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->grabMouse();
        }
    }

    void nativeUngrabMouse(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->ungrabMouse();
        }
    }

    void nativeGetCursorPos(UObject* object, Frame& stack, void*)
    {
        float* outX = vm::getRef<float>(stack);
        float* outY = vm::getRef<float>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        URootWindow* rootWindow = window ? window->root() : nullptr;
        if (rootWindow) {
            convertCoordinates(rootWindow, rootWindow->mouseX(), rootWindow->mouseY(), window, *outX, *outY);
        } else {
            *outX = 0;
            *outY = 0;
        }
    }

    void nativeSetCursorPos(UObject* object, Frame& stack, void*)
    {
        float newX = vm::get<float>(stack);
        float newY = vm::get<float>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        URootWindow* rootWindow = window ? window->root() : nullptr;
        if (rootWindow) {
            float rx = 0;
            float ry = 0;
            convertCoordinates(window, newX, newY, rootWindow, rx, ry);
            rootWindow->setRootCursorPos(rx, ry);
        }
    }

    void nativeSetDefaultCursor(UObject* object, Frame& stack, void*)
    {
        UObject* texture = vm::get<UObject*>(stack);
        UObject* shadow = nullptr;
        float hotX = -1;
        float hotY = -1;
        Color color {};
        bool hasShadow = optional(stack, shadow);
        optional(stack, hotX);
        optional(stack, hotY);
        bool hasColor = optional(stack, color);
        vm::finish(stack);
        UWindow* window = self(object);
        if (!window) {
            return;
        }
        window::defaultCursor(window) = texture;
        if (hasShadow) {
            window::defaultCursorShadow(window) = shadow;
        }
        window::defaultHotX(window) = hotX;
        window::defaultHotY(window) = hotY;
        if (hasColor) {
            window::defaultCursorColor(window) = color;
        }
    }
}

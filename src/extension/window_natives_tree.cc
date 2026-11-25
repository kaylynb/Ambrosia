#include "extension/windows.h"

#include "extension/window_natives.h"

namespace dx::extension {
    void nativeDestroy(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->destroyWindowTree();
        }
    }

    void nativeNewChild(UObject* object, Frame& stack, void* result)
    {
        UObject* cls = vm::get<UObject*>(stack);
        bool show = optionalBoolDefault(stack, true);
        vm::finish(stack);
        UWindow* window = self(object);
        UWindow* child = window ? window->newChild(cast<UClass>(cls), show) : nullptr;
        vm::ret<UObject*>(result, child);
    }

    void nativeRaise(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        UWindow* owner = window ? window->parent() : nullptr;
        if (owner) {
            window->detachFromParent();
            window->attachLast(owner);
        }
    }

    void nativeLower(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        UWindow* owner = window ? window->parent() : nullptr;
        if (owner) {
            window->detachFromParent();
            window->attachFirst(owner);
        }
    }

    void nativeShow(UObject* object, Frame& stack, void*)
    {
        bool value = optionalBoolDefault(stack, true);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->show(value);
        }
    }

    void nativeHide(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->show(false);
        }
    }

    namespace {
        bool anyVisible(UWindow* window)
        {
            if (window->visible()) {
                return true;
            }
            for (UWindow* child = window->firstChild(); child; child = child->nextSibling()) {
                if (anyVisible(child)) {
                    return true;
                }
            }
            return false;
        }

        bool anySensitive(UWindow* window)
        {
            if (window->sensitive()) {
                return true;
            }
            for (UWindow* child = window->firstChild(); child; child = child->nextSibling()) {
                if (anySensitive(child)) {
                    return true;
                }
            }
            return false;
        }
    }

    void nativeIsVisible(UObject* object, Frame& stack, void* result)
    {
        bool recurse = optionalBoolDefault(stack, true);
        vm::finish(stack);
        UWindow* window = self(object);
        bool value = false;
        if (window) {
            value = window->visible();
            if (value && recurse) {
                for (UWindow* current = window->parent(); current; current = current->parent()) {
                    if (!current->visible()) {
                        value = false;
                    }
                }
            }
        }
        vm::retBool(result, value);
    }

    void nativeSetSensitivity(UObject* object, Frame& stack, void*)
    {
        bool value = vm::getBool(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->setSensitivity(value);
        }
    }

    void nativeEnableWindow(UObject* object, Frame& stack, void*)
    {
        bool value = optionalBoolDefault(stack, true);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->setSensitivity(value);
        }
    }

    void nativeDisableWindow(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->setSensitivity(false);
        }
    }

    void nativeIsSensitive(UObject* object, Frame& stack, void* result)
    {
        bool recurse = optionalBoolDefault(stack, true);
        vm::finish(stack);
        UWindow* window = self(object);
        bool value = false;
        if (window) {
            value = window->sensitive();
            if (value && recurse) {
                for (UWindow* current = window->parent(); current; current = current->parent()) {
                    if (!current->sensitive()) {
                        value = false;
                    }
                }
            }
        }
        vm::retBool(result, value);
    }

    void nativeSetSelectability(UObject* object, Frame& stack, void*)
    {
        bool value = vm::getBool(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        if (window && (window::bIsSelectable(window) != 0) != value) {
            window::bIsSelectable(window) = value ? 1 : 0;
            if (URootWindow* rootWindow = window->root()) {
                rootWindow->checkFocusWindow();
            }
        }
    }

    void nativeSetBackground(UObject* object, Frame& stack, void*)
    {
        UObject* texture = vm::get<UObject*>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::Background(window) = texture;
            window::bDrawRawBackground(window) = 1;
        }
    }

    void nativeSetBackgroundStyle(UObject* object, Frame& stack, void*)
    {
        uint8_t value = vm::get<uint8_t>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::backgroundStyle(window) = value;
        }
    }

    void nativeSetBackgroundSmoothing(UObject* object, Frame& stack, void*)
    {
        bool value = vm::getBool(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::bSmoothBackground(window) = value ? 1 : 0;
        }
    }

    void nativeSetBackgroundStretching(UObject* object, Frame& stack, void*)
    {
        bool value = vm::getBool(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::bStretchBackground(window) = value ? 1 : 0;
        }
    }

    void nativeGetRootWindow(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<UObject*>(result, window ? window->root() : nullptr);
    }

    void nativeGetModalWindow(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<UObject*>(result, window ? window->modalWindow() : nullptr);
    }

    void nativeGetTabGroupWindow(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* found = nullptr;
        for (UWindow* current = self(object); current; current = current->parent()) {
            if (cast<UTabGroupWindow>(current)) {
                found = current;
                break;
            }
        }
        vm::ret<UObject*>(result, found);
    }

    void nativeGetParent(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<UObject*>(result, window ? window->parent() : nullptr);
    }

    void nativeGetPlayerPawn(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<UObject*>(result, window ? window->playerPawn() : nullptr);
    }

    namespace {
        void nativeChildSibling(UObject* object, Frame& stack, void* result, int mode)
        {
            bool visibleOnly = optionalBoolDefault(stack, true);
            vm::finish(stack);
            UWindow* window = self(object);
            UWindow* found = nullptr;
            if (window) {
                UWindow* current = nullptr;
                if (mode == 0) {
                    current = window->lastChild();
                } else if (mode == 1) {
                    current = window->firstChild();
                } else if (mode == 2) {
                    current = window->nextSibling();
                } else {
                    current = window->prevSibling();
                }
                bool forward = mode == 1 || mode == 2;
                while (current) {
                    if (!visibleOnly || current->visible()) {
                        found = current;
                        break;
                    }
                    current = forward ? current->nextSibling() : current->prevSibling();
                }
            }
            vm::ret<UObject*>(result, found);
        }
    }

    void nativeGetTopChild(UObject* object, Frame& stack, void* result)
    {
        nativeChildSibling(object, stack, result, 0);
    }

    void nativeGetBottomChild(UObject* object, Frame& stack, void* result)
    {
        nativeChildSibling(object, stack, result, 1);
    }

    void nativeGetHigherSibling(UObject* object, Frame& stack, void* result)
    {
        nativeChildSibling(object, stack, result, 2);
    }

    void nativeGetLowerSibling(UObject* object, Frame& stack, void* result)
    {
        nativeChildSibling(object, stack, result, 3);
    }

    void nativeDestroyAllChildren(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        if (!window) {
            return;
        }
        while (UWindow* child = window->firstChild()) {
            child->destroyWindowTree();
            if (window->firstChild() == child) {
                child->detachFromParent();
            }
        }
    }
}

#include "extension/windows.h"

#include "engine/input.h"

#include <cctype>

namespace dx::extension {
    namespace {
        bool isMouseKey(uint8_t key)
        {
            return key == IK_MouseX || key == IK_MouseY || key == IK_MouseZ || key == IK_MouseW || key == IK_LeftMouse || key == IK_RightMouse || key == IK_MiddleMouse || key == IK_MouseWheelUp || key == IK_MouseWheelDown;
        }

        bool chainSensitive(UWindow* window)
        {
            for (UWindow* current = window; current; current = current->parent()) {
                if (!current->sensitive()) {
                    return false;
                }
            }
            return true;
        }

        UWindow* acceleratorWindow(UWindow* window, int wanted)
        {
            int32_t accel = window::acceleratorKey(window);
            if (accel > 0 && accel < 255 && std::toupper(accel) == wanted && window->selectable() && window->effectivelyVisible() && chainSensitive(window)) {
                return window;
            }
            for (UWindow* child = window->firstChild(); child; child = child->nextSibling()) {
                if (child->destroyed || cast<UModalWindow>(child) || !chainSensitive(child)) {
                    continue;
                }
                if (UWindow* found = acceleratorWindow(child, wanted)) {
                    return found;
                }
            }
            return nullptr;
        }
    }

    void UModalWindow::initWindow()
    {
        modalwindow::focusMode(this) = 2;
        UTabGroupWindow::initWindow();
    }

    bool UModalWindow::keyPressed(const std::string& key)
    {
        if (UTabGroupWindow::keyPressed(key)) {
            return true;
        }
        if (key.empty() || !isCurrentModal() || !state().keys[IK_Alt]) {
            return false;
        }
        UWindow* target = acceleratorWindow(this, std::toupper(static_cast<unsigned char>(key[0])));
        return target && target->acceleratorKeyPressed(key);
    }

    void UModalWindow::visibilityChanged(bool value)
    {
        State& s = state();
        if (value) {
            // Count the grab before the script handler runs, so a handler that
            // hides this modal again releases the grab it just took.
            if (parent() && root()) {
                if (s.keyboardGrabs <= 0) {
                    for (size_t key = 0; key < s.keys.size(); key++) {
                        if (!isMouseKey(static_cast<uint8_t>(key))) {
                            s.keys[key] = false;
                        }
                    }
                }
                s.keyboardGrabs++;
            }
            UTabGroupWindow::visibilityChanged(value);
            return;
        }
        UTabGroupWindow::visibilityChanged(value);
        if (!parent() || !root()) {
            return;
        }
        if (s.keyboardGrabs > 0) {
            s.keyboardGrabs--;
        }
    }

    bool UModalWindow::isCurrentModal()
    {
        URootWindow* rootWindow = root();
        return rootWindow && rootWindow->topModal() == this;
    }
}

#include "extension/windows.h"

#include "engine/input.h"
#include "extension/window_internal.h"

#include <algorithm>
#include <cmath>

namespace dx::extension {
    namespace {
        UWindow* commonAncestor(UWindow* a, UWindow* b)
        {
            for (UWindow* first = a; first; first = first->parent()) {
                for (UWindow* second = b; second; second = second->parent()) {
                    if (first == second) {
                        return first;
                    }
                }
            }
            return nullptr;
        }
    }

    UWindow* URootWindow::cursorFocus(float& relativeX, float& relativeY)
    {
        UWindow* grabbed = windowOf(rootwindow::grabbedWindow(this));
        if (grabbed) {
            float gx = 0;
            float gy = 0;
            grabbed->absolutePosition(gx, gy);
            relativeX = mouseX() + usedX - gx;
            relativeY = mouseY() + usedY - gy;
            return grabbed;
        }
        // XRootWindow::GetMouseWindow: the search starts at the
        // topmost visible modal (or the root), and a point over none of its windows goes to
        // the modal itself, so nothing behind it can be clicked.
        UWindow* modal = topModal();
        float mx = 0;
        float my = 0;
        modal->absolutePosition(mx, my);
        float px = mouseX() + usedX - mx;
        float py = mouseY() + usedY - my;
        UWindow* found = findWindowByPoint(modal, px, py, relativeX, relativeY);
        if (!found) {
            relativeX = px;
            relativeY = py;
            return modal;
        }
        return found;
    }

    UWindow* URootWindow::findWindowByPoint(UWindow* window, float px, float py, float& relativeX, float& relativeY)
    {
        // XWindow::FindWindowByPoint: only a visible, sensitive window holding
        // the point counts, so insensitive windows pass clicks to their parent; children are
        // tried from the top, skipping modal windows nested inside.
        if (!window->visible() || !window->sensitive() || px < 0 || py < 0 || px >= window->width() || py >= window->height()) {
            return nullptr;
        }
        for (UWindow* child = window->lastChild(); child; child = child->prevSibling()) {
            if (cast<UModalWindow>(child)) {
                continue;
            }
            if (UWindow* found = findWindowByPoint(child, px - child->usedX, py - child->usedY, relativeX, relativeY)) {
                return found;
            }
        }
        relativeX = px;
        relativeY = py;
        return window;
    }

    void URootWindow::setRootCursorPos(float newMouseX, float newMouseY)
    {
        float scale = virtualScale();
        float realWidth = std::ceil(state().viewportWidth / scale);
        float realHeight = std::ceil(state().viewportHeight / scale);
        newMouseX = std::clamp(newMouseX + usedX, 0.0f, realWidth) - usedX;
        newMouseY = std::clamp(newMouseY + usedY, 0.0f, realHeight) - usedY;
        rootwindow::prevMouseX(this) = mouseX();
        rootwindow::prevMouseY(this) = mouseY();
        mouseX() = newMouseX;
        mouseY() = newMouseY;
        rootwindow::bMouseMoved(this) = 1;

        float relativeX = 0;
        float relativeY = 0;
        UWindow* focus = cursorFocus(relativeX, relativeY);
        UWindow* last = windowOf(rootwindow::lastMouseWindow(this));
        if (last != focus) {
            uint8_t mouseFocusMode = modalwindow::focusMode(topModal());
            UWindow* ancestor = commonAncestor(last, focus);
            for (UWindow* current = last; current && current != ancestor; current = current->parent()) {
                current->mouseLeftWindow();
            }
            if (last && mouseFocusMode >= 3) {
                setRootFocusWindow(nullptr);
            }
            rootwindow::lastMouseWindow(this) = focus;
            std::vector<UWindow*> entered;
            for (UWindow* current = focus; current && current != ancestor; current = current->parent()) {
                entered.push_back(current);
            }
            for (auto it = entered.rbegin(); it != entered.rend(); ++it) {
                (*it)->mouseEnteredWindow();
            }
            if (focus && mouseFocusMode >= 2) {
                setRootFocusWindow(focus);
            }
        }
        if (focus && !focus->destroyed) {
            focus->mouseMoved(relativeX, relativeY);
        }
    }

    bool URootWindow::onRawMouseMove(float dx, float dy)
    {
        if (isCursorVisible()) {
            float speed = 1.0f / virtualScale();
            setRootCursorPos(mouseX() + dx * speed, mouseY() + dy * speed);
        }
        return isModalOpen();
    }

    bool URootWindow::onMouseButton(uint8_t key, bool pressed)
    {
        float relativeX = 0;
        float relativeY = 0;
        UWindow* focus = cursorFocus(relativeX, relativeY);
        if (!focus) {
            return isModalOpen();
        }
        // XRootWindow::HandleButtons: a press grabs the
        // mouse for the window under it and a release lets go of the window that
        // has it, before any event is sent, so a window keeps the mouse only while
        // a button is held. (A script's GrabMouse during the press moves the grab;
        // the release still ends it.) A press also moves the focus to the clicked
        // window, or its nearest selectable ancestor, whenever the top modal's
        // focus mode is not MFOCUS_None.
        if (pressed) {
            focus->grabMouse();
            if (modalwindow::focusMode(topModal()) != 0) {
                setRootFocusWindow(focus);
            }
        } else {
            focus->ungrabMouse();
        }
        int32_t numClicks = countClick(focus, key, pressed);
        if (!pressed) {
            const MouseReleaseHook& releaseHook = state().mouseReleaseHook;
            if (releaseHook && releaseHook(focus, key)) {
                if (cast<UButtonWindow>(focus)) {
                    buttonwindow::bMousePressed(focus) = 0;
                    buttonwindow::bButtonPressed(focus) = 0;
                }
                return true;
            }
        }
        // Each window from the one under the mouse up to the root gets the raw event
        // first, then its pressed or released event.
        float px = relativeX;
        float py = relativeY;
        for (UWindow* current = focus; current; current = current->parent()) {
            if (current->destroyed) {
                break;
            }
            if (current->rawMouseButtonPressed(px, py, key, pressed ? IST_Press : IST_Release)) {
                return true;
            }
            if (pressed ? current->mouseButtonPressed(px, py, key, numClicks) : current->mouseButtonReleased(px, py, key, numClicks)) {
                return true;
            }
            px += current->usedX;
            py += current->usedY;
        }
        return isModalOpen();
    }

    int32_t URootWindow::countClick(UWindow* window, uint8_t key, bool pressed)
    {
        // XRootWindow::HandleButtons: a press or release continues the run of
        // clicks only on the same window and button, before the deadline set by
        // the last press, and with the mouse within maxMouseDist of the run's
        // first press on each axis. Anything else starts a new run.
        double now = state().time;
        float maxDistance = rootwindow::maxMouseDist(this);
        bool continues = lastClickWindow == window && lastClickButton == key && clickDeadline > now && std::fabs(mouseX() - firstClickX) <= maxDistance && std::fabs(mouseY() - firstClickY) <= maxDistance;
        if (!continues) {
            clicks = 0;
            clickDeadline = -1;
            lastClickButton = key;
            lastClickWindow = window;
        }
        if (pressed) {
            if (continues) {
                clicks++;
            }
            clickDeadline = now + static_cast<double>(rootwindow::multiClickTimeout(this));
            if (clicks < 1) {
                firstClickX = mouseX();
                firstClickY = mouseY();
            }
        }
        rootwindow::clickCount(this) = clicks;
        return clicks + 1;
    }

    bool URootWindow::onKey(uint8_t key, bool pressed, bool repeat)
    {
        UWindow* start = focusWindow();
        if (!start) {
            for (UWindow* child = lastChild(); child; child = child->prevSibling()) {
                if (child->visible() && cast<UModalWindow>(child)) {
                    start = child;
                    break;
                }
            }
        }
        if (!start) {
            start = this;
        }
        if (!pressed) {
            for (UWindow* current = start; current; current = current->parent()) {
                if (current->rawKeyPressed(key, IST_Release, false)) {
                    return true;
                }
            }
            return isModalOpen();
        }
        for (UWindow* current = start; current; current = current->parent()) {
            if (current->destroyed) {
                break;
            }
            if (!repeat && current->rawKeyPressed(key, IST_Press, false)) {
                return true;
            }
            if (current->virtualKeyPressed(key, repeat)) {
                return true;
            }
        }
        return isModalOpen();
    }

    bool URootWindow::onChar(const std::string& chars)
    {
        UWindow* start = focusWindow();
        if (!start) {
            start = topModal();
        }
        bool alt = state().keys[IK_Alt];
        for (UWindow* current = start; current; current = current->parent()) {
            if (current->destroyed) {
                break;
            }
            if (alt && !cast<UModalWindow>(current)) {
                continue;
            }
            if (current->keyPressed(chars)) {
                return true;
            }
        }
        return isModalOpen();
    }
}

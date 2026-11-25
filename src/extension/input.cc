#include "extension/windows.h"

#include "engine/input.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace dx::extension {
    namespace {
        // The vertical scale of a scroll area when it is shown, meaning there is more to see.
        UScaleWindow* shownScale(UWindow* window)
        {
            UScrollAreaWindow* area = cast<UScrollAreaWindow>(window);
            UScaleWindow* scale = area ? cast<UScaleWindow>(scrollareawindow::vScale(area)) : nullptr;
            for (UWindow* current = scale; current; current = current->parent()) {
                if (!current->visible()) {
                    return nullptr;
                }
            }
            return scale;
        }

        void collectScrollable(UWindow* window, std::vector<UWindow*>& out)
        {
            for (UWindow* child = window->firstChild(); child; child = child->nextSibling()) {
                if (!child->visible()) {
                    continue;
                }
                if (shownScale(child)) {
                    out.push_back(child);
                }
                collectScrollable(child, out);
            }
        }

        bool contains(UWindow* area, UWindow* window)
        {
            for (UWindow* current = window; current; current = current->parent()) {
                if (current == area) {
                    return true;
                }
            }
            return false;
        }

        // The scroll areas the right stick runs through, top to bottom. Focus moving through a
        // list already scrolls it, so panels without one (a conversation's text, the goals and
        // the notes) come first: the stick reads them wherever the focus is. With none, it is
        // the scroll area around the focused control, or every scrollable panel.
        std::vector<UWindow*> scrollAreas()
        {
            State& s = state();
            UWindow* modal = menuOpen() ? s.root->topModal() : nullptr;
            if (!modal) {
                return {};
            }
            std::vector<UWindow*> areas;
            collectScrollable(modal, areas);
            if (areas.empty()) {
                return areas;
            }
            std::vector<UWindow*> candidates = s.root->navigationCandidates();
            std::vector<UWindow*> reading;
            for (UWindow* area : areas) {
                if (std::none_of(candidates.begin(), candidates.end(), [area](UWindow* candidate) { return cast<UListWindow>(candidate) && contains(area, candidate); })) {
                    reading.push_back(area);
                }
            }
            if (reading.empty()) {
                for (UWindow* current = s.root->focusWindow(); current; current = current->parent()) {
                    if (shownScale(current)) {
                        reading.push_back(current);
                        break;
                    }
                }
            }
            if (reading.empty()) {
                reading = std::move(areas);
            }
            auto top = [](UWindow* window) {
                float x = 0;
                float y = 0;
                window->absolutePosition(x, y);
                return std::pair { y, x };
            };
            std::stable_sort(reading.begin(), reading.end(), [&](UWindow* a, UWindow* b) { return top(a) < top(b); });
            return reading;
        }

        // Down scrolls the first panel not yet at its end, up the last one not at its start,
        // so several panels read as one page.
        UScaleWindow* scrollTarget(bool up)
        {
            std::vector<UWindow*> areas = scrollAreas();
            if (areas.empty()) {
                return nullptr;
            }
            auto movable = [up](UWindow* area) {
                UScaleWindow* scale = shownScale(area);
                int32_t position = scalewindow::currentPos(scale);
                return up ? position > 0 : position < scale->maxPosition();
            };
            if (up) {
                auto found = std::find_if(areas.rbegin(), areas.rend(), movable);
                return shownScale(found == areas.rend() ? areas.front() : *found);
            }
            auto found = std::find_if(areas.begin(), areas.end(), movable);
            return shownScale(found == areas.end() ? areas.back() : *found);
        }
    }

    std::string focusReport()
    {
        State& s = state();
        if (!s.root || s.root->destroyed) {
            return "no root window";
        }
        UWindow* modal = s.root->topModal();
        auto describe = [](UWindow* window) {
            float x = 0;
            float y = 0;
            window->absolutePosition(x, y);
            return std::format("{} ({}) at {},{} {}x{}", window->name.str(), window->cls->name.str(), x, y, window->width(), window->height());
        };
        std::string out = std::format("screen {}\n", describe(modal));
        UWindow* focus = s.root->focusWindow();
        out += std::format("focus {}\n", focus ? describe(focus) : "none");
        std::vector<UWindow*> candidates = s.root->navigationCandidates();
        for (UWindow* window : candidates) {
            out += "  reachable " + describe(window) + "\n";
        }
        // Shown, sensitive buttons and lists the pad cannot get to.
        std::vector<UWindow*> pending { modal };
        while (!pending.empty()) {
            UWindow* window = pending.back();
            pending.pop_back();
            if (!window->visible()) {
                continue;
            }
            bool control = cast<UButtonWindow>(window) || cast<UListWindow>(window) || cast<UEditWindow>(window);
            if (control && window->sensitive() && std::find(candidates.begin(), candidates.end(), window) == candidates.end()) {
                out += "  unreachable " + describe(window) + "\n";
            }
            for (UWindow* child = window->firstChild(); child; child = child->nextSibling()) {
                pending.push_back(child);
            }
        }
        for (UWindow* area : scrollAreas()) {
            out += "  scrolls " + describe(area) + "\n";
        }
        return out;
    }

    bool canScroll()
    {
        if (auto* modal = cast<UModalWindow>(topModal()); modal && modal->scrollContent(0)) {
            return true;
        }
        return !scrollAreas().empty();
    }

    bool scroll(bool up)
    {
        if (auto* modal = cast<UModalWindow>(topModal()); modal && modal->scrollContent(up ? -1 : 1)) {
            return true;
        }
        UScaleWindow* scale = scrollTarget(up);
        if (!scale) {
            return false;
        }
        scale->moveThumb(up ? 4 : 5);
        return true;
    }

    void useGamepad()
    {
        State& s = state();
        if (s.root && !s.root->destroyed) {
            s.root->gamepadFocus = true;
        }
    }

    bool keyEvent(uint8_t key, uint8_t action, float delta)
    {
        State& s = state();
        if (action == IST_Axis) {
            if (!s.root || s.root->destroyed) {
                return false;
            }
            if (key == IK_MouseX) {
                return s.root->onRawMouseMove(delta, 0);
            }
            if (key == IK_MouseY) {
                return s.root->onRawMouseMove(0, -delta);
            }
            return s.root->isModalOpen();
        }
        bool pressed = action == IST_Press || action == IST_Hold;
        bool wasDown = s.keys[key];
        if (action == IST_Press) {
            s.keys[key] = true;
        } else if (action == IST_Release) {
            s.keys[key] = false;
            if (!wasDown) {
                return s.root && !s.root->destroyed && s.root->isModalOpen();
            }
        }
        if (key == IK_LeftMouse || key == IK_RightMouse || key == IK_MiddleMouse || key == IK_MouseWheelUp || key == IK_MouseWheelDown) {
            if (action == IST_Hold) {
                return s.root && !s.root->destroyed && s.root->isModalOpen();
            }
            return mouseButton(key, pressed);
        }
        if (!s.root || s.root->destroyed || s.keyboardGrabs <= 0) {
            return false;
        }
        return s.root->onKey(key, pressed, action == IST_Hold);
    }

    bool charEvent(char character)
    {
        State& s = state();
        if (!s.root || s.root->destroyed || s.keyboardGrabs <= 0) {
            return false;
        }
        return s.root->onChar(std::string(1, character));
    }

    bool navigate(uint8_t key)
    {
        State& s = state();
        if (!s.root || s.root->destroyed || s.keyboardGrabs <= 0) {
            return false;
        }
        s.root->gamepadFocus = true;
        UWindow* focus = s.root->focusWindow();
        if (auto* scale = cast<UScaleWindow>(focus)) {
            // Screen scripts can consume all four arrows (e.g. to pan a camera).
            // On a slider the pad adjusts along its axis and leaves across it.
            if (scale->stepAlongAxis(key)) {
                return true;
            }
        } else if (focus && !cast<UEditWindow>(focus)) {
            // A list at its first or last row keeps the key; let focus leave it instead.
            UListWindow* list = cast<UListWindow>(focus);
            int32_t row = list ? listwindow::focusLine(list) : 0;
            bool handled = focus->rawKeyPressed(key, IST_Press, false) || focus->virtualKeyPressed(key, false);
            if (handled && (!list || listwindow::focusLine(list) != row)) {
                return true;
            }
        }
        int32_t dx = key == IK_Left ? -1 : (key == IK_Right ? 1 : 0);
        int32_t dy = key == IK_Up ? -1 : (key == IK_Down ? 1 : 0);
        std::vector<UWindow*> candidates = s.root->navigationCandidates();
        if (std::find(candidates.begin(), candidates.end(), focus) == candidates.end()) {
            // Start from the screen's own controls rather than a skipped window.
            s.root->setRootFocusWindow(nullptr);
            dx = 0;
            dy = 0;
        }
        s.root->moveFocusToward(dx, dy);
        return true;
    }

    void mouseMove(float x, float y)
    {
        State& s = state();
        if (!s.root || s.root->destroyed || !s.root->isCursorVisible()) {
            return;
        }
        float scale = virtualScale();
        // Warping the OS cursor onto the UI cursor reports a move to where it already is;
        // only a real move hands control back to the mouse.
        if (std::abs(x / scale - s.root->usedX - s.root->mouseX()) > 1.0f || std::abs(y / scale - s.root->usedY - s.root->mouseY()) > 1.0f) {
            s.root->gamepadFocus = false;
        }
        s.root->setRootCursorPos(x / scale - s.root->usedX, y / scale - s.root->usedY);
    }

    bool cursorPosition(float& x, float& y)
    {
        State& s = state();
        if (!s.root || s.root->destroyed) {
            return false;
        }
        float scale = virtualScale();
        x = (s.root->mouseX() + s.root->usedX) * scale;
        y = (s.root->mouseY() + s.root->usedY) * scale;
        return true;
    }

    bool mouseButton(uint8_t key, bool pressed)
    {
        State& s = state();
        if (pressed) {
            s.keys[key] = true;
        } else {
            s.keys[key] = false;
        }
        if (!s.root || s.root->destroyed) {
            return false;
        }
        if (key == IK_MouseWheelUp || key == IK_MouseWheelDown) {
            if (!pressed) {
                return s.root->isModalOpen();
            }
            if (auto* modal = cast<UModalWindow>(topModal()); modal && modal->scrollContent(key == IK_MouseWheelUp ? -1 : 1)) {
                return true;
            }
            bool handled = s.root->onMouseButton(key, true);
            s.root->onMouseButton(key, false);
            return handled;
        }
        return s.root->onMouseButton(key, pressed);
    }
}

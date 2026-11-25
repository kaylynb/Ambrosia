#include "extension/windows.h"

#include "extension/window_internal.h"

#include <algorithm>
#include <cmath>

namespace dx::extension {
    namespace {
        void collectFocusable(UWindow* window, std::vector<UWindow*>& out)
        {
            for (UWindow* child = window->firstChild(); child; child = child->nextSibling()) {
                if (!child->visible() || !child->sensitive()) {
                    continue;
                }
                if (child->selectable()) {
                    out.push_back(child);
                }
                collectFocusable(child, out);
            }
        }

        UWindow* tabGroupOf(UWindow* window)
        {
            for (UWindow* current = window; current; current = current->parent()) {
                if (cast<UTabGroupWindow>(current)) {
                    return current;
                }
            }
            return nullptr;
        }

        void collectGroupMembers(UWindow* window, std::vector<UWindow*>& out)
        {
            for (UWindow* child = window->firstChild(); child; child = child->nextSibling()) {
                if (child->destroyed || cast<UTabGroupWindow>(child)) {
                    continue;
                }
                if (child->selectable()) {
                    out.push_back(child);
                }
                collectGroupMembers(child, out);
            }
        }

        struct Located {
            UWindow* window;
            float x;
            float y;
        };

        std::vector<UWindow*> groupOrder(UWindow* group, bool rowMajor)
        {
            std::vector<UWindow*> members;
            if (group->selectable()) {
                members.push_back(group);
            }
            collectGroupMembers(group, members);
            std::vector<Located> located;
            for (UWindow* member : members) {
                Located entry { member, 0, 0 };
                member->absolutePosition(entry.x, entry.y);
                located.push_back(entry);
            }
            std::stable_sort(located.begin(), located.end(), [rowMajor](const Located& a, const Located& b) {
                if (rowMajor) {
                    return a.y != b.y ? a.y < b.y : a.x < b.x;
                }
                return a.x != b.x ? a.x < b.x : a.y < b.y;
            });
            std::vector<UWindow*> ordered;
            for (const Located& entry : located) {
                ordered.push_back(entry.window);
            }
            return ordered;
        }

        void collectTabGroups(UWindow* window, std::vector<UWindow*>& out)
        {
            for (UWindow* child = window->firstChild(); child; child = child->nextSibling()) {
                if (child->destroyed || cast<UModalWindow>(child)) {
                    continue;
                }
                if (cast<UTabGroupWindow>(child)) {
                    out.push_back(child);
                }
                collectTabGroups(child, out);
            }
        }
    }

    float& URootWindow::mouseX()
    {
        return rootwindow::MouseX(this);
    }

    float& URootWindow::mouseY()
    {
        return rootwindow::MouseY(this);
    }

    UWindow* URootWindow::focusWindow()
    {
        return windowOf(rootwindow::FocusWindow(this));
    }

    UWindow* URootWindow::topModal()
    {
        for (UWindow* child = lastChild(); child; child = child->prevSibling()) {
            if (child->visible() && cast<UModalWindow>(child)) {
                return child;
            }
        }
        return this;
    }

    bool URootWindow::isTraversable(UWindow* window, bool checkModal)
    {
        if (!window || !window->selectable()) {
            return false;
        }
        for (UWindow* current = window; current; current = current->parent()) {
            if (!current->visible() || !current->sensitive()) {
                return false;
            }
        }
        return !checkModal || window->modalWindow() == topModal();
    }

    UWindow* URootWindow::moveTabGroup(int32_t move)
    {
        UWindow* modal = topModal();
        std::vector<UWindow*> groups { modal };
        collectTabGroups(modal, groups);
        std::vector<Located> located;
        for (UWindow* group : groups) {
            Located entry { group, 0, 0 };
            for (UWindow* member : groupOrder(group, true)) {
                if (member->width() > 0 && member->height() > 0 && isTraversable(member, false)) {
                    member->absolutePosition(entry.x, entry.y);
                    break;
                }
            }
            located.push_back(entry);
        }
        std::stable_sort(located.begin(), located.end(), [](const Located& a, const Located& b) {
            return a.y != b.y ? a.y < b.y : a.x < b.x;
        });
        int32_t count = static_cast<int32_t>(located.size());
        if (count <= 0) {
            return focusWindow();
        }
        bool back = move == 0 || move == 2;
        int32_t start = 0;
        UWindow* focus = focusWindow();
        if (focus && focus->modalWindow() == modal) {
            UWindow* current = tabGroupOf(focus);
            // Visit every control in a group before moving to the next group. Otherwise
            // a button bar contributes only its first button to keyboard tab navigation.
            std::vector<UWindow*> members = groupOrder(current, true);
            auto focused = std::find(members.begin(), members.end(), focus);
            if (focused != members.end()) {
                int32_t at = static_cast<int32_t>(focused - members.begin()) + (back ? -1 : 1);
                for (; at >= 0 && at < static_cast<int32_t>(members.size()); at += back ? -1 : 1) {
                    if (isTraversable(members[static_cast<size_t>(at)], false)) {
                        setRootFocusWindow(members[static_cast<size_t>(at)]);
                        return focusWindow();
                    }
                }
            }
            for (int32_t i = 0; i < count; i++) {
                if (located[static_cast<size_t>(i)].window == current) {
                    start = back ? (i - 1 < 0 ? count - 1 : i - 1) : (i + 1 >= count ? 0 : i + 1);
                    break;
                }
            }
        }
        int32_t index = start;
        UWindow* target = nullptr;
        do {
            std::vector<UWindow*> members = groupOrder(located[static_cast<size_t>(index)].window, true);
            if (back) {
                std::reverse(members.begin(), members.end());
            }
            for (UWindow* member : members) {
                if (isTraversable(member, false)) {
                    target = member;
                    break;
                }
            }
            if (target) {
                break;
            }
            index = back ? (index - 1 < 0 ? count - 1 : index - 1) : (index + 1 >= count ? 0 : index + 1);
        } while (index != start);
        if (target) {
            setRootFocusWindow(target);
        }
        return focusWindow();
    }

    UWindow* URootWindow::moveFocus(int32_t move)
    {
        UWindow* focus = focusWindow();
        if (!focus) {
            moveTabGroup(move);
            return focusWindow();
        }
        UWindow* group = tabGroupOf(focus);
        if (!group) {
            return focus;
        }
        std::vector<UWindow*> list = groupOrder(group, move == 0 || move == 1);
        int32_t count = static_cast<int32_t>(list.size());
        int32_t index = static_cast<int32_t>(std::find(list.begin(), list.end(), focus) - list.begin());
        if (index >= count) {
            moveTabGroup(move);
            return focusWindow();
        }
        bool back = move == 0 || move == 2;
        for (int32_t remaining = count; remaining > 0; remaining--) {
            index = back ? (index - 1 < 0 ? count - 1 : index - 1) : (index + 1 >= count ? 0 : index + 1);
            if (isTraversable(list[static_cast<size_t>(index)], true)) {
                setRootFocusWindow(list[static_cast<size_t>(index)]);
                return focusWindow();
            }
        }
        moveTabGroup(move);
        return focusWindow();
    }

    std::vector<UWindow*> URootWindow::navigationCandidates()
    {
        std::vector<UWindow*> candidates;
        UWindow* modal = topModal();
        if (!modal) {
            return candidates;
        }
        collectFocusable(modal, candidates);
        const auto& skip = state().navigationSkip;
        std::erase_if(candidates, [this, &skip](UWindow* window) {
            return !isTraversable(window, true) || window->width() <= 0 || window->height() <= 0 || (skip && skip(window));
        });
        return candidates;
    }

    UWindow* URootWindow::moveFocusToward(int32_t dx, int32_t dy)
    {
        UWindow* modal = topModal();
        std::vector<UWindow*> candidates = navigationCandidates();
        if (candidates.empty()) {
            return focusWindow();
        }
        auto center = [](UWindow* window) {
            float x = 0;
            float y = 0;
            window->absolutePosition(x, y);
            return std::pair { x + window->width() * 0.5f, y + window->height() * 0.5f };
        };
        UWindow* focus = focusWindow();
        if (!focus || std::find(candidates.begin(), candidates.end(), focus) == candidates.end()) {
            UWindow* preferred = windowOf(modalwindow::preferredFocus(modal));
            if (preferred && std::find(candidates.begin(), candidates.end(), preferred) != candidates.end()) {
                setRootFocusWindow(preferred);
                return focusWindow();
            }
            // Otherwise start from the top-left control.
            UWindow* first = *std::min_element(candidates.begin(), candidates.end(), [&](UWindow* a, UWindow* b) {
                auto [ax, ay] = center(a);
                auto [bx, by] = center(b);
                return ay + ax * 0.25f < by + bx * 0.25f;
            });
            setRootFocusWindow(first);
            return focusWindow();
        }

        // Edges along the direction of travel and across it, so "down" reads as "past my
        // bottom edge" whatever the controls' sizes.
        struct Span {
            float nearEdge;
            float farEdge;
            float acrossLow;
            float acrossHigh;
        };

        auto span = [dx, dy](UWindow* window) {
            float x = 0;
            float y = 0;
            window->absolutePosition(x, y);
            float sign = static_cast<float>(dx != 0 ? dx : dy);
            float low = dx != 0 ? x : y;
            float high = low + (dx != 0 ? window->width() : window->height());
            float acrossLow = dx != 0 ? y : x;
            float acrossHigh = acrossLow + (dx != 0 ? window->height() : window->width());
            // Flip so that "further along" is always a larger number.
            return sign > 0 ? Span { low, high, acrossLow, acrossHigh } : Span { -high, -low, acrossLow, acrossHigh };
        };
        Span from = span(focus);
        float fromCenter = (from.acrossLow + from.acrossHigh) * 0.5f;
        // The control nearest along the direction of travel. Wrapped, it is the one furthest
        // back instead, for coming in at the other end of the screen.
        auto nearest = [&](bool wrapped) {
            UWindow* best = nullptr;
            float bestScore = 0;
            for (UWindow* candidate : candidates) {
                if (candidate == focus) {
                    continue;
                }
                Span to = span(candidate);
                // Must start past the current control's far edge; neighboring buttons in a
                // grid overlap their borders by a unit or two.
                float along = to.nearEdge - from.farEdge;
                if (!wrapped && along < -2.0f) {
                    continue;
                }
                // Sideways distance is the gap between the controls, zero when they line up.
                float gap = std::max({ 0.0f, to.acrossLow - from.acrossHigh, from.acrossLow - to.acrossHigh });
                float offset = std::abs((to.acrossLow + to.acrossHigh) * 0.5f - fromCenter);
                float score = (wrapped ? to.nearEdge : std::max(along, 0.0f)) + gap * 4.0f + offset * 0.1f;
                if (!best || score < bestScore) {
                    best = candidate;
                    bestScore = score;
                }
            }
            return best;
        };
        UWindow* best = nearest(false);
        // Off the top or bottom of a screen that wraps, come back in at the other end, in line
        // with the current control.
        const NavigationWrap& wrap = state().navigationWrap;
        if (!best && dy != 0 && wrap && wrap(modal)) {
            best = nearest(true);
        }
        if (best) {
            setRootFocusWindow(best);
        }
        return focusWindow();
    }

    void URootWindow::checkFocusWindow()
    {
        UWindow* old = focusWindow();
        if (!old || isTraversable(old, true)) {
            return;
        }
        UWindow* preferred = windowOf(modalwindow::preferredFocus(topModal()));
        if (preferred && isTraversable(preferred, true)) {
            setRootFocusWindow(preferred);
        }
        if (focusWindow() == old && moveFocus(1) == old) {
            setRootFocusWindow(nullptr);
        }
    }

    void URootWindow::tick(float deltaSeconds)
    {
        if (!focusWindow()) {
            UWindow* modal = topModal();
            if (modalwindow::focusMode(modal) < 3) {
                updateLayout();
                UWindow* preferred = windowOf(modalwindow::preferredFocus(modal));
                if (preferred && isTraversable(preferred, true)) {
                    setRootFocusWindow(preferred);
                }
                if (!focusWindow()) {
                    moveTabGroup(3);
                }
            }
        }
        UModalWindow::tick(deltaSeconds);
    }

    bool URootWindow::isModalOpen()
    {
        for (UWindow* child = lastChild(); child; child = child->prevSibling()) {
            if (child->visible() && cast<UModalWindow>(child)) {
                return true;
            }
        }
        return false;
    }

    bool URootWindow::isCursorVisible()
    {
        return isModalOpen() && rootwindow::bCursorVisible(this) != 0;
    }

    void URootWindow::forgetWindow(UWindow* window)
    {
        if (rootwindow::FocusWindow(this) == window) {
            rootwindow::FocusWindow(this) = nullptr;
        }
        if (rootwindow::grabbedWindow(this) == window) {
            rootwindow::grabbedWindow(this) = nullptr;
        }
        if (rootwindow::lastMouseWindow(this) == window) {
            rootwindow::lastMouseWindow(this) = nullptr;
        }
        if (lastClickWindow == window) {
            lastClickWindow = nullptr;
        }
    }

    bool URootWindow::setRootFocusWindow(UWindow* newFocus)
    {
        if (newFocus) {
            UWindow* selectableAncestor = newFocus;
            while (selectableAncestor && !selectableAncestor->selectable()) {
                selectableAncestor = selectableAncestor->parent();
            }
            if (selectableAncestor) {
                newFocus = selectableAncestor;
            }
        }
        UWindow* oldFocus = focusWindow();
        if (oldFocus == newFocus) {
            return true;
        }
        if (newFocus) {
            if (!isTraversable(newFocus, true)) {
                return false;
            }
            if (UWindow* modal = newFocus->modalWindow()) {
                modalwindow::preferredFocus(modal) = newFocus;
            }
        }
        rootwindow::FocusWindow(this) = newFocus;
        if (oldFocus) {
            oldFocus->focusLeftWindow();
            oldFocus->playWindowSound(window::unfocusSound(oldFocus));
            for (UWindow* current = oldFocus; current; current = current->parent()) {
                current->focusLeftDescendant(oldFocus);
            }
        }
        if (newFocus && !newFocus->destroyed && focusWindow() == newFocus) {
            newFocus->askParentToShowArea(0, 0, newFocus->width(), newFocus->height());
            newFocus->playWindowSound(window::focusSound(newFocus));
            newFocus->focusEnteredWindow();
            for (UWindow* current = newFocus; current; current = current->parent()) {
                current->focusEnteredDescendant(newFocus);
            }
        }
        return true;
    }
}

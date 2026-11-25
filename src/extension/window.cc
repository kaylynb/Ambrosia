#include "extension/windows.h"

#include "audio/audio.h"
#include "core/config.h"
#include "core/log.h"
#include "core/objects.h"
#include "engine/input.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "extension/window_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace dx::extension {
    UWindow* windowOf(UObject* object)
    {
        UWindow* window = cast<UWindow>(object);
        if (!window || window->destroyed) {
            return nullptr;
        }
        return window;
    }

    bool attachedTo(UWindow* window, UWindow* owner)
    {
        return window && !window->destroyed && window->parent() == owner;
    }

    // The child to visit after `child` ran script, given the sibling cached
    // before it ran and the last earlier child that stayed attached.
    UWindow* nextChild(UWindow* owner, UWindow* child, UWindow* following, UWindow* previous)
    {
        if (!following || attachedTo(following, owner)) {
            return following;
        }
        if (attachedTo(child, owner)) {
            return child->nextSibling();
        }
        if (attachedTo(previous, owner)) {
            return previous->nextSibling();
        }
        return previous ? nullptr : owner->firstChild();
    }

    UWindow* UWindow::parent()
    {
        return cast<UWindow>(window::parentOwner(this));
    }

    UWindow* UWindow::firstChild()
    {
        return cast<UWindow>(window::firstChild(this));
    }

    UWindow* UWindow::lastChild()
    {
        return cast<UWindow>(window::lastChild(this));
    }

    UWindow* UWindow::nextSibling()
    {
        return cast<UWindow>(window::nextSibling(this));
    }

    UWindow* UWindow::prevSibling()
    {
        return cast<UWindow>(window::prevSibling(this));
    }

    bool UWindow::visible()
    {
        return window::bIsVisible(this) != 0;
    }

    bool UWindow::sensitive()
    {
        return window::bIsSensitive(this) != 0;
    }

    bool UWindow::selectable()
    {
        return window::bIsSelectable(this) != 0;
    }

    float& UWindow::x()
    {
        return window::X(this);
    }

    float& UWindow::y()
    {
        return window::Y(this);
    }

    float& UWindow::width()
    {
        return window::Width(this);
    }

    float& UWindow::height()
    {
        return window::Height(this);
    }

    URootWindow* UWindow::root()
    {
        UWindow* current = this;
        while (current->parent()) {
            current = current->parent();
        }
        return cast<URootWindow>(current);
    }

    UObject* UWindow::playerPawn()
    {
        URootWindow* rootWindow = root();
        return rootWindow ? rootwindow::parentPawn(rootWindow) : nullptr;
    }

    UWindow* UWindow::modalWindow()
    {
        for (UWindow* current = this; current; current = current->parent()) {
            if (cast<UModalWindow>(current)) {
                return current;
            }
        }
        return nullptr;
    }

    void UWindow::absolutePosition(float& outX, float& outY)
    {
        outX = 0;
        outY = 0;
        for (UWindow* current = this; current; current = current->parent()) {
            outX += current->usedX;
            outY += current->usedY;
        }
    }

    void UWindow::detachFromParent()
    {
        UWindow* owner = parent();
        UWindow* previous = prevSibling();
        UWindow* following = nextSibling();
        if (previous) {
            window::nextSibling(previous) = following;
        }
        if (following) {
            window::prevSibling(following) = previous;
        }
        if (owner) {
            if (owner->firstChild() == this) {
                window::firstChild(owner) = following;
            }
            if (owner->lastChild() == this) {
                window::lastChild(owner) = previous;
            }
        }
        window::prevSibling(this) = nullptr;
        window::nextSibling(this) = nullptr;
        window::parentOwner(this) = nullptr;
        window::winParent(this) = nullptr;
    }

    void UWindow::attachLast(UWindow* owner)
    {
        window::parentOwner(this) = owner;
        window::winParent(this) = owner;
        window::prevSibling(this) = owner->lastChild();
        window::nextSibling(this) = nullptr;
        if (owner->lastChild()) {
            window::nextSibling(owner->lastChild()) = this;
        }
        window::lastChild(owner) = this;
        if (!owner->firstChild()) {
            window::firstChild(owner) = this;
        }
    }

    void UWindow::attachFirst(UWindow* owner)
    {
        window::parentOwner(this) = owner;
        window::winParent(this) = owner;
        window::nextSibling(this) = owner->firstChild();
        window::prevSibling(this) = nullptr;
        if (owner->firstChild()) {
            window::prevSibling(owner->firstChild()) = this;
        }
        window::firstChild(owner) = this;
        if (!owner->lastChild()) {
            window::lastChild(owner) = this;
        }
    }

    UWindow* UWindow::newChild(UClass* childClass, bool show)
    {
        if (!childClass) {
            return nullptr;
        }
        UObject* object = objects::newObject(childClass, objects::packageObject(Name("Transient")), Name(), RF_Transient);
        objects::flushLoads();
        UWindow* child = cast<UWindow>(object);
        if (!child) {
            log::error("DevUI", "{} is not a window class", childClass->name.str());
            return nullptr;
        }
        child->attachLast(this);
        window::firstChild(child) = nullptr;
        window::lastChild(child) = nullptr;
        window::bIsVisible(child) = 0;
        window::bIsSensitive(child) = 1;
        // XWindow::Init starts every window at 10x10,
        // the size a preferred-size query falls back to until it is configured.
        child->width() = 10.0f;
        child->height() = 10.0f;
        // XWindow::Init sends ChildAdded to the parent and
        // DescendantAdded to the parent and every ancestor before
        // XWindow::CreateNewWindow runs the script InitWindow.
        childAdded(child);
        for (UWindow* ancestor = this; ancestor && !child->destroyed; ancestor = ancestor->parent()) {
            ancestor->descendantAdded(child);
        }
        if (child->destroyed) {
            // The original goes on with a freed window here and crashes.
            return nullptr;
        }
        child->initWindow();
        if (child->destroyed) {
            // CreateNewWindow returns None for a child its own InitWindow
            // destroyed. The original's window is locked, so it is only
            // detached at this point: it still shows it and asks it to
            // reconfigure (on a window with no parent) before freeing it.
            // Ambrosia has already destroyed it and skips those steps.
            return nullptr;
        }
        window::bIsInitialized(child) = 1;
        if (show) {
            child->show(true);
        }
        // CreateNewWindow asks for reconfigure on the new child, not the parent.
        if (!child->destroyed) {
            child->askParentForReconfigure();
        }
        window::bNeedsReconfigure(this) = 1;
        return child->destroyed ? nullptr : child;
    }

    void UWindow::destroyWindowTree()
    {
        if (window::bBeingDestroyed(this) || destroyed) {
            return;
        }
        window::bBeingDestroyed(this) = 1;
        if (effectivelyVisible()) {
            show(false);
        }
        while (UWindow* child = firstChild()) {
            child->destroyWindowTree();
            if (firstChild() == child) {
                child->detachFromParent();
            }
        }
        destroyWindow();
        URootWindow* rootWindow = root();
        UWindow* owner = parent();
        if (UWindow* modal = owner ? owner->modalWindow() : nullptr) {
            if (modalwindow::preferredFocus(modal) == this) {
                modalwindow::preferredFocus(modal) = nullptr;
            }
        }
        detachFromParent();
        if (owner) {
            owner->childRemoved(this);
            for (UWindow* ancestor = owner; ancestor; ancestor = ancestor->parent()) {
                ancestor->descendantRemoved(this);
            }
            window::bNeedsReconfigure(owner) = 1;
        }
        if (rootWindow) {
            rootWindow->forgetWindow(this);
        }
        if (state().root == this) {
            state().root = nullptr;
        }
        timers.clear();
        if (!destroyed) {
            state().destroyedWindows.push_back(this);
        }
        objects::destroyObject(this);
    }

    void UWindow::show(bool value)
    {
        if (visible() == value) {
            return;
        }
        if (UWindow* owner = parent()) {
            owner->childRequestedVisibilityChange(this, value);
        } else {
            setChildVisibility(value);
        }
    }

    bool UWindow::effectivelyVisible()
    {
        for (UWindow* current = this; current; current = current->parent()) {
            if (!current->visible()) {
                return false;
            }
        }
        return true;
    }

    void UWindow::setChildVisibility(bool value)
    {
        if (visible() == value) {
            return;
        }
        bool before = effectivelyVisible();
        window::bIsVisible(this) = value ? 1 : 0;
        bool after = effectivelyVisible();
        if (before != after) {
            if (URootWindow* rootWindow = root()) {
                rootWindow->checkFocusWindow();
            }
            invokeVisibilityChange(after);
        }
        if (!destroyed) {
            askParentForReconfigure();
        }
    }

    void UWindow::invokeVisibilityChange(bool value)
    {
        visibilityChanged(value);
        if (destroyed) {
            return;
        }
        playWindowSound(value ? window::visibleSound(this) : window::invisibleSound(this));
        UWindow* child = firstChild();
        UWindow* previous = nullptr;
        while (child) {
            UWindow* following = child->nextSibling();
            if (!child->destroyed && child->visible()) {
                child->invokeVisibilityChange(value);
            }
            UWindow* next = nextChild(this, child, following, previous);
            if (attachedTo(child, this)) {
                previous = child;
            }
            child = next;
        }
    }

    void UWindow::playWindowSound(UObject* sound)
    {
        if (sound) {
            float volume = window::SoundVolume(this);
            playSound(this, sound, volume > 0 ? volume : 1.0f, 1.0f);
        }
    }

    void UWindow::setSensitivity(bool value)
    {
        if (sensitive() != value) {
            window::bIsSensitive(this) = value ? 1 : 0;
            if (URootWindow* rootWindow = root()) {
                rootWindow->checkFocusWindow();
            }
            sensitivityChanged(value);
        }
    }

    bool UWindow::setFocusWindow(UWindow* window)
    {
        URootWindow* rootWindow = root();
        return rootWindow && rootWindow->setRootFocusWindow(window);
    }

    void UWindow::grabMouse()
    {
        if (URootWindow* rootWindow = root()) {
            rootwindow::grabbedWindow(rootWindow) = this;
        }
    }

    void UWindow::ungrabMouse()
    {
        if (URootWindow* rootWindow = root()) {
            if (rootwindow::grabbedWindow(rootWindow) == this) {
                rootwindow::grabbedWindow(rootWindow) = nullptr;
            }
        }
    }

    void UWindow::setFont(UObject* font)
    {
        window::normalFont(this) = font;
        window::boldFont(this) = font;
    }
}

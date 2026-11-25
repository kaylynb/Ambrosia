#include "extension/windows.h"

#include "extension/window_internal.h"

#include <algorithm>

namespace dx::extension {
    namespace {
        bool runBool(vm::Call& call)
        {
            if (!call.valid()) {
                return false;
            }
            call.run();
            uint8_t* result = call.returnValue();
            return result && *reinterpret_cast<uint32_t*>(result) != 0;
        }
    }

    void runVoid(vm::Call& call)
    {
        if (call.valid()) {
            call.run();
        }
    }

    bool UWindow::sendButtonActivated(UWindow* button)
    {
        const ButtonHook& hook = state().buttonHook;
        if (hook && hook(button)) {
            return true;
        }
        for (UWindow* current = this; current; current = current->parent()) {
            if (current->buttonActivated(button)) {
                return true;
            }
        }
        return false;
    }

    bool UWindow::sendButtonActivatedRight(UWindow* button)
    {
        for (UWindow* current = this; current; current = current->parent()) {
            if (current->buttonActivatedRight(button)) {
                return true;
            }
        }
        return false;
    }

    void UWindow::initWindow()
    {
        window::TextColor(this) = { 0, 255, 0, 255 };
        window::tileColor(this) = { 255, 255, 255, 255 };
        window::textVSpacing(this) = 1.0f;
        window::bSpecialText(this) = 1;
        baselineOffset = 2.0f;
        underlineHeight = 1.0f;
        window::backgroundStyle(this) = DSTY_Normal;
        static const Name eventName("InitWindow");
        vm::Call call(this, eventName);
        runVoid(call);
    }

    void UWindow::destroyWindow()
    {
        static const Name eventName("DestroyWindow");
        vm::Call call(this, eventName);
        runVoid(call);
    }

    void UWindow::windowReady()
    {
        static const Name eventName("WindowReady");
        vm::Call call(this, eventName);
        runVoid(call);
    }

    void UWindow::parentRequestedPreferredSize(bool widthSpecified, float& preferredWidth, bool heightSpecified, float& preferredHeight)
    {
        static const Name eventName("ParentRequestedPreferredSize");
        vm::Call call(this, eventName);
        if (!call.valid()) {
            return;
        }
        call.b(widthSpecified).f(preferredWidth).b(heightSpecified).f(preferredHeight);
        call.run();
        preferredWidth = *reinterpret_cast<float*>(call.parameter(1));
        preferredHeight = *reinterpret_cast<float*>(call.parameter(3));
    }

    void UWindow::parentRequestedGranularity(float& hGranularity, float& vGranularity)
    {
        static const Name eventName("ParentRequestedGranularity");
        vm::Call call(this, eventName);
        if (!call.valid()) {
            return;
        }
        call.f(hGranularity).f(vGranularity);
        call.run();
        hGranularity = *reinterpret_cast<float*>(call.parameter(0));
        vGranularity = *reinterpret_cast<float*>(call.parameter(1));
    }

    bool UWindow::childRequestedReconfiguration(UWindow* child)
    {
        static const Name eventName("ChildRequestedReconfiguration");
        vm::Call call(this, eventName);
        call.obj(child);
        return runBool(call);
    }

    void UWindow::childRequestedShowArea(UWindow* child, float showX, float showY, float showWidth, float showHeight)
    {
        static const Name eventName("ChildRequestedShowArea");
        vm::Call call(this, eventName);
        if (!call.valid()) {
            askParentToShowArea(child->x() + showX, child->y() + showY, showWidth, showHeight);
            return;
        }
        call.obj(child).f(showX).f(showY).f(showWidth).f(showHeight);
        call.run();
    }

    void UWindow::childRequestedVisibilityChange(UWindow* child, bool visible)
    {
        static const Name eventName("ChildRequestedVisibilityChange");
        vm::Call call(this, eventName);
        call.obj(child).b(visible);
        runVoid(call);
    }

    void UWindow::configurationChanged()
    {
        static const Name eventName("ConfigurationChanged");
        vm::Call call(this, eventName);
        runVoid(call);
    }

    void UWindow::visibilityChanged(bool value)
    {
        static const Name eventName("VisibilityChanged");
        vm::Call call(this, eventName);
        call.b(value);
        runVoid(call);
    }

    void UWindow::sensitivityChanged(bool value)
    {
        static const Name eventName("SensitivityChanged");
        vm::Call call(this, eventName);
        call.b(value);
        runVoid(call);
    }

    void UWindow::mouseMoved(float newX, float newY)
    {
        static const Name eventName("MouseMoved");
        vm::Call call(this, eventName);
        call.f(newX).f(newY);
        runVoid(call);
    }

    void UWindow::mouseEnteredWindow()
    {
        static const Name eventName("MouseEnteredWindow");
        vm::Call call(this, eventName);
        runVoid(call);
    }

    void UWindow::mouseLeftWindow()
    {
        static const Name eventName("MouseLeftWindow");
        vm::Call call(this, eventName);
        runVoid(call);
    }

    bool UWindow::rawMouseButtonPressed(float pointX, float pointY, uint8_t button, uint8_t inputState)
    {
        static const Name eventName("RawMouseButtonPressed");
        vm::Call call(this, eventName);
        call.f(pointX).f(pointY).byte(button).byte(inputState);
        return runBool(call);
    }

    bool UWindow::mouseButtonPressed(float pointX, float pointY, uint8_t button, int32_t numClicks)
    {
        static const Name eventName("MouseButtonPressed");
        vm::Call call(this, eventName);
        call.f(pointX).f(pointY).byte(button).i(numClicks);
        return runBool(call);
    }

    bool UWindow::mouseButtonReleased(float pointX, float pointY, uint8_t button, int32_t numClicks)
    {
        static const Name eventName("MouseButtonReleased");
        vm::Call call(this, eventName);
        call.f(pointX).f(pointY).byte(button).i(numClicks);
        return runBool(call);
    }

    bool UWindow::rawKeyPressed(uint8_t key, uint8_t inputState, bool repeat)
    {
        static const Name eventName("RawKeyPressed");
        vm::Call call(this, eventName);
        call.byte(key).byte(inputState).b(repeat);
        return runBool(call);
    }

    bool UWindow::keyPressed(const std::string& key)
    {
        static const Name eventName("KeyPressed");
        vm::Call call(this, eventName);
        call.s(key);
        return runBool(call);
    }

    bool UWindow::virtualKeyPressed(uint8_t key, bool repeat)
    {
        static const Name eventName("VirtualKeyPressed");
        vm::Call call(this, eventName);
        call.byte(key).b(repeat);
        return runBool(call);
    }

    bool UWindow::acceleratorKeyPressed(const std::string& key)
    {
        static const Name eventName("AcceleratorKeyPressed");
        vm::Call call(this, eventName);
        call.s(key);
        return runBool(call);
    }

    void UWindow::focusEnteredWindow()
    {
        static const Name eventName("FocusEnteredWindow");
        vm::Call call(this, eventName);
        runVoid(call);
    }

    void UWindow::focusLeftWindow()
    {
        static const Name eventName("FocusLeftWindow");
        vm::Call call(this, eventName);
        runVoid(call);
    }

    void UWindow::focusEnteredDescendant(UWindow* window)
    {
        static const Name eventName("FocusEnteredDescendant");
        vm::Call call(this, eventName);
        call.obj(window);
        runVoid(call);
    }

    void UWindow::focusLeftDescendant(UWindow* window)
    {
        static const Name eventName("FocusLeftDescendant");
        vm::Call call(this, eventName);
        call.obj(window);
        runVoid(call);
    }

    bool UWindow::buttonActivated(UWindow* button)
    {
        static const Name eventName("ButtonActivated");
        vm::Call call(this, eventName);
        call.obj(button);
        return runBool(call);
    }

    bool UWindow::buttonActivatedRight(UWindow* button)
    {
        static const Name eventName("ButtonActivatedRight");
        vm::Call call(this, eventName);
        call.obj(button);
        return runBool(call);
    }

    bool UWindow::toggleChanged(UWindow* button, bool newToggle)
    {
        static const Name eventName("ToggleChanged");
        vm::Call call(this, eventName);
        call.obj(button).b(newToggle);
        return runBool(call);
    }

    bool UWindow::boxOptionSelected(UWindow* box, int32_t buttonNumber)
    {
        static const Name eventName("BoxOptionSelected");
        vm::Call call(this, eventName);
        call.obj(box).i(buttonNumber);
        return runBool(call);
    }

    bool UWindow::scalePositionChanged(UWindow* scale, int32_t newTickPosition, float newValue, bool final)
    {
        static const Name eventName("ScalePositionChanged");
        vm::Call call(this, eventName);
        call.obj(scale).i(newTickPosition).f(newValue).b(final);
        return runBool(call);
    }

    bool UWindow::scaleRangeChanged(UWindow* scale, int32_t fromTick, int32_t toTick, float fromValue, float toValue, bool final)
    {
        static const Name eventName("ScaleRangeChanged");
        vm::Call call(this, eventName);
        call.obj(scale).i(fromTick).i(toTick).f(fromValue).f(toValue).b(final);
        return runBool(call);
    }

    bool UWindow::scaleAttributesChanged(UWindow* scale, int32_t tickPosition, int32_t tickSpan, int32_t numTicks)
    {
        static const Name eventName("ScaleAttributesChanged");
        vm::Call call(this, eventName);
        call.obj(scale).i(tickPosition).i(tickSpan).i(numTicks);
        return runBool(call);
    }

    bool UWindow::clipAttributesChanged(UWindow* clip, int32_t newClipWidth, int32_t newClipHeight, int32_t newChildWidth, int32_t newChildHeight)
    {
        static const Name eventName("ClipAttributesChanged");
        vm::Call call(this, eventName);
        call.obj(clip).i(newClipWidth).i(newClipHeight).i(newChildWidth).i(newChildHeight);
        return runBool(call);
    }

    bool UWindow::clipPositionChanged(UWindow* clip, int32_t newCol, int32_t newRow)
    {
        static const Name eventName("ClipPositionChanged");
        vm::Call call(this, eventName);
        call.obj(clip).i(newCol).i(newRow);
        return runBool(call);
    }

    void UWindow::queryGranularity(float& hGranularity, float& vGranularity)
    {
        hGranularity = 1.0f;
        vGranularity = 1.0f;
        parentRequestedGranularity(hGranularity, vGranularity);
        hGranularity = std::max(hGranularity, 1.0f);
        vGranularity = std::max(vGranularity, 1.0f);
    }

    bool UWindow::listRowActivated(UWindow* list, int32_t rowId)
    {
        static const Name eventName("ListRowActivated");
        vm::Call call(this, eventName);
        call.obj(list).i(rowId);
        return runBool(call);
    }

    bool UWindow::listSelectionChanged(UWindow* list, int32_t numSelections, int32_t focusRowId)
    {
        static const Name eventName("ListSelectionChanged");
        vm::Call call(this, eventName);
        call.obj(list).i(numSelections).i(focusRowId);
        return runBool(call);
    }

    bool UWindow::textChanged(UWindow* edit, bool modified)
    {
        static const Name eventName("TextChanged");
        vm::Call call(this, eventName);
        call.obj(edit).b(modified);
        return runBool(call);
    }

    bool UWindow::editActivated(UWindow* edit, bool modified)
    {
        static const Name eventName("EditActivated");
        vm::Call call(this, eventName);
        call.obj(edit).b(modified);
        return runBool(call);
    }

    void UWindow::childAdded(UWindow* child)
    {
        static const Name eventName("ChildAdded");
        vm::Call call(this, eventName);
        call.obj(child);
        runVoid(call);
    }

    void UWindow::childRemoved(UWindow* child)
    {
        static const Name eventName("ChildRemoved");
        vm::Call call(this, eventName);
        call.obj(child);
        runVoid(call);
    }

    void UWindow::descendantAdded(UWindow* descendant)
    {
        static const Name eventName("DescendantAdded");
        vm::Call call(this, eventName);
        call.obj(descendant);
        runVoid(call);
    }

    void UWindow::descendantRemoved(UWindow* descendant)
    {
        static const Name eventName("DescendantRemoved");
        vm::Call call(this, eventName);
        call.obj(descendant);
        runVoid(call);
    }

    void UWindow::tick(float deltaSeconds)
    {
        if (window::bTickEnabled(this)) {
            static const Name eventName("Tick");
            vm::Call call(this, eventName);
            call.f(deltaSeconds);
            runVoid(call);
        }
        if (destroyed) {
            return;
        }
        // XWindow::InvokeTimers: a timer fires when its
        // time left drops below zero. A looping timer catches up on every period
        // it missed with one call that says how many; the function gets the timer
        // id, that count and the client data.
        std::vector<int32_t> expired;
        for (auto& [id, timer] : timers) {
            timer.left -= deltaSeconds;
            if (timer.left < 0) {
                expired.push_back(id);
            }
        }
        for (int32_t id : expired) {
            auto it = timers.find(id);
            if (it == timers.end()) {
                continue;
            }
            Name function = it->second.function;
            int32_t clientData = it->second.clientData;
            int32_t invocations = 1;
            if (it->second.loop) {
                invocations = static_cast<int32_t>(-it->second.left / it->second.timeout) + 1;
                it->second.left += static_cast<float>(invocations) * it->second.timeout;
            } else {
                timers.erase(it);
            }
            vm::Call call(this, function);
            if (call.valid()) {
                call.i(id).i(invocations).i(clientData);
                call.run();
            }
            if (destroyed) {
                return;
            }
        }
        UWindow* child = firstChild();
        UWindow* previous = nullptr;
        while (child) {
            UWindow* following = child->nextSibling();
            child->tick(deltaSeconds);
            UWindow* next = nextChild(this, child, following, previous);
            if (attachedTo(child, this)) {
                previous = child;
            }
            child = next;
        }
    }
}

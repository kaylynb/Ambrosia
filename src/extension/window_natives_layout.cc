#include "extension/windows.h"

#include "extension/window_natives.h"

#include <algorithm>

namespace dx::extension {
    void nativeSetConfiguration(UObject* object, Frame& stack, void*)
    {
        float newX = vm::get<float>(stack);
        float newY = vm::get<float>(stack);
        float newWidth = vm::get<float>(stack);
        float newHeight = vm::get<float>(stack);
        vm::finish(stack);
        // XWindow::Configure: left and top aligned at
        // the given position, with a hardcoded size (negative sizes become 0).
        if (UWindow* window = self(object)) {
            window::winHAlign(window) = HALIGN_Left;
            window::winVAlign(window) = VALIGN_Top;
            window::hMargin0(window) = newX;
            window::vMargin0(window) = newY;
            window::bConfigured(window) = 0;
            window->setWidth(std::max(newWidth, 0.0f));
            window->setHeight(std::max(newHeight, 0.0f));
        }
    }

    void nativeSetSize(UObject* object, Frame& stack, void*)
    {
        float newWidth = vm::get<float>(stack);
        float newHeight = vm::get<float>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->setWidth(newWidth);
            window->setHeight(newHeight);
        }
    }

    void nativeSetPos(UObject* object, Frame& stack, void*)
    {
        float newX = vm::get<float>(stack);
        float newY = vm::get<float>(stack);
        vm::finish(stack);
        // XWindow::Move: SetPos makes the window
        // left and top aligned, with the position as its first margins.
        if (UWindow* window = self(object)) {
            window::winHAlign(window) = HALIGN_Left;
            window::winVAlign(window) = VALIGN_Top;
            window::hMargin0(window) = newX;
            window::vMargin0(window) = newY;
            window::bConfigured(window) = 0;
            window::bNeedsReconfigure(window) = 1;
            window->askParentForReconfigure();
        }
    }

    void nativeSetWidth(UObject* object, Frame& stack, void*)
    {
        float value = vm::get<float>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->setWidth(value);
        }
    }

    void nativeSetHeight(UObject* object, Frame& stack, void*)
    {
        float value = vm::get<float>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->setHeight(value);
        }
    }

    void nativeResetSize(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->fixedWidth = false;
            window->fixedHeight = false;
            window::bNeedsReconfigure(window) = 1;
            window->askParentForReconfigure();
        }
    }

    void nativeResetWidth(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->fixedWidth = false;
            window::bNeedsReconfigure(window) = 1;
            window->askParentForReconfigure();
        }
    }

    void nativeResetHeight(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->fixedHeight = false;
            window::bNeedsReconfigure(window) = 1;
            window->askParentForReconfigure();
        }
    }

    void nativeSetWindowAlignments(UObject* object, Frame& stack, void*)
    {
        uint8_t halign = vm::get<uint8_t>(stack);
        uint8_t valign = vm::get<uint8_t>(stack);
        float h0 = 0;
        float v0 = 0;
        float h1 = 0;
        float v1 = 0;
        bool hasH0 = optional(stack, h0);
        bool hasV0 = optional(stack, v0);
        bool hasH1 = optional(stack, h1);
        bool hasV1 = optional(stack, v1);
        vm::finish(stack);
        UWindow* window = self(object);
        if (!window) {
            return;
        }
        // execSetWindowAlignments: left-out first
        // margins are 0 and left-out second margins copy the first.
        window::winHAlign(window) = halign;
        window::winVAlign(window) = valign;
        window::hMargin0(window) = hasH0 ? h0 : 0.0f;
        window::vMargin0(window) = hasV0 ? v0 : 0.0f;
        window::hMargin1(window) = hasH1 ? h1 : window::hMargin0(window);
        window::vMargin1(window) = hasV1 ? v1 : window::vMargin0(window);
        window::bConfigured(window) = 0;
        window::bNeedsReconfigure(window) = 1;
        window->askParentForReconfigure();
    }

    void nativeAskParentForReconfigure(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->askParentForReconfigure();
        }
    }

    void nativeConfigureChild(UObject* object, Frame& stack, void*)
    {
        float newX = vm::get<float>(stack);
        float newY = vm::get<float>(stack);
        float newWidth = vm::get<float>(stack);
        float newHeight = vm::get<float>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->configureChild(newX, newY, newWidth, newHeight);
        }
    }

    void nativeResizeChild(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        // XWindow::ResizeChild places the window by its alignment again, even
        // if its parent had placed it with ConfigureChild.
        if (UWindow* window = self(object)) {
            window::bConfigured(window) = 0;
            window::bNeedsQuery(window) = 1;
            window::bNeedsReconfigure(window) = 1;
        }
    }

    void nativeQueryPreferredWidth(UObject* object, Frame& stack, void* result)
    {
        float queryHeight = vm::get<float>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<float>(result, window ? window->queryPreferredWidth(queryHeight) : 0.0f);
    }

    void nativeQueryPreferredHeight(UObject* object, Frame& stack, void* result)
    {
        float queryWidth = vm::get<float>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        vm::ret<float>(result, window ? window->queryPreferredHeight(queryWidth) : 0.0f);
    }

    void nativeQueryPreferredSize(UObject* object, Frame& stack, void*)
    {
        float* outWidth = vm::getRef<float>(stack);
        float* outHeight = vm::getRef<float>(stack);
        vm::finish(stack);
        UWindow* window = self(object);
        float w = 0;
        float h = 0;
        if (window) {
            window->queryPreferredSize(w, h);
        }
        *outWidth = w;
        *outHeight = h;
    }

    void nativeQueryGranularity(UObject* object, Frame& stack, void*)
    {
        float* outH = vm::getRef<float>(stack);
        float* outV = vm::getRef<float>(stack);
        vm::finish(stack);
        float h = 1;
        float v = 1;
        if (UWindow* window = self(object)) {
            window->parentRequestedGranularity(h, v);
        }
        *outH = h;
        *outV = v;
    }

    void nativeSetChildVisibility(UObject* object, Frame& stack, void*)
    {
        bool value = vm::getBool(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->setChildVisibility(value);
        }
    }

    void nativeAskParentToShowArea(UObject* object, Frame& stack, void*)
    {
        float areaX = 0;
        float areaY = 0;
        float areaWidth = 0;
        float areaHeight = 0;
        UWindow* window = self(object);
        if (window) {
            areaWidth = window->width();
            areaHeight = window->height();
        }
        optional(stack, areaX);
        optional(stack, areaY);
        optional(stack, areaWidth);
        optional(stack, areaHeight);
        vm::finish(stack);
        if (window) {
            window->askParentToShowArea(areaX, areaY, areaWidth, areaHeight);
        }
    }
}

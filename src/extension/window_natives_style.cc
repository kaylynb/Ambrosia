#include "extension/windows.h"

#include "core/objects.h"
#include "extension/window_natives.h"

namespace dx::extension {
    void nativePlaySound(UObject* object, Frame& stack, void*)
    {
        UObject* sound = vm::get<UObject*>(stack);
        float volume = 1;
        float pitch = 1;
        float posX = 0;
        float posY = 0;
        optional(stack, volume);
        optional(stack, pitch);
        optional(stack, posX);
        optional(stack, posY);
        vm::finish(stack);
        playSound(self(object), sound, volume, pitch);
    }

    void nativeSetSoundVolume(UObject* object, Frame& stack, void*)
    {
        float value = vm::get<float>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::SoundVolume(window) = value;
        }
    }

    void nativeSetTileColor(UObject* object, Frame& stack, void*)
    {
        Color value = vm::get<Color>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::tileColor(window) = value;
        }
    }

    void nativeSetTextColor(UObject* object, Frame& stack, void*)
    {
        Color value = vm::get<Color>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::TextColor(window) = value;
        }
    }

    void nativeSetFont(UObject* object, Frame& stack, void*)
    {
        UObject* font = vm::get<UObject*>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window->setFont(font);
            window->askParentForReconfigure();
        }
    }

    void nativeSetFonts(UObject* object, Frame& stack, void*)
    {
        UObject* normal = vm::get<UObject*>(stack);
        UObject* bold = vm::get<UObject*>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::normalFont(window) = normal;
            window::boldFont(window) = bold;
            window->askParentForReconfigure();
        }
    }

    void nativeSetNormalFont(UObject* object, Frame& stack, void*)
    {
        UObject* font = vm::get<UObject*>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::normalFont(window) = font;
            window->askParentForReconfigure();
        }
    }

    void nativeSetBoldFont(UObject* object, Frame& stack, void*)
    {
        UObject* font = vm::get<UObject*>(stack);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::boldFont(window) = font;
        }
    }

    void nativeEnableSpecialText(UObject* object, Frame& stack, void*)
    {
        bool value = optionalBoolDefault(stack, true);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::bSpecialText(window) = value ? 1 : 0;
        }
    }

    void nativeEnableTranslucentText(UObject* object, Frame& stack, void*)
    {
        bool value = optionalBoolDefault(stack, true);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            window::bTextTranslucent(window) = value ? 1 : 0;
        }
    }

    void nativeCarriageReturn(UObject*, Frame& stack, void* result)
    {
        vm::finish(stack);
        vm::ret<std::string>(result, "\n");
    }

    void nativeSetBaselineData(UObject* object, Frame& stack, void*)
    {
        float baseline = 0;
        float underline = 0;
        bool hasBaseline = optional(stack, baseline);
        bool hasUnderline = optional(stack, underline);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            if (hasBaseline) {
                window->baselineOffset = baseline;
            }
            if (hasUnderline) {
                window->underlineHeight = underline;
            }
        }
    }

    void nativeGetGC(UObject* object, Frame& stack, void* result)
    {
        vm::finish(stack);
        State& s = state();
        if (!s.windowGc) {
            UClass* cls = extensionClass("Extension.GC");
            if (cls) {
                s.windowGc = cast<UGC>(objects::newObject(cls, objects::packageObject(Name("Transient")), Name("WindowGC"), RF_Transient));
            }
        }
        UWindow* window = self(object);
        if (s.windowGc && window) {
            float x = 0;
            float y = 0;
            for (UWindow* current = window; current; current = current->parent()) {
                x += current->usedX;
                y += current->usedY;
            }
            s.windowGc->resetState(window, x, y);
        }
        vm::ret<UObject*>(result, s.windowGc);
    }

    void nativeReleaseGC(UObject*, Frame& stack, void*)
    {
        vm::get<UObject*>(stack);
        vm::finish(stack);
    }

    void nativeChangeStyle(UObject* object, Frame& stack, void*)
    {
        vm::finish(stack);
        UWindow* window = self(object);
        if (!window) {
            return;
        }
        UWindow* top = window;
        while (top->parent()) {
            top = top->parent();
        }
        std::vector<UWindow*> stackWindows { top };
        while (!stackWindows.empty()) {
            UWindow* current = stackWindows.back();
            stackWindows.pop_back();
            static const Name eventName("StyleChanged");
            vm::Call call(current, eventName);
            if (call.valid()) {
                call.run();
            }
            for (UWindow* child = current->firstChild(); child; child = child->nextSibling()) {
                stackWindows.push_back(child);
            }
        }
    }

    void nativeSetFocusSounds(UObject* object, Frame& stack, void*)
    {
        UObject* focus = nullptr;
        UObject* unfocus = nullptr;
        bool hasFocus = optional(stack, focus);
        bool hasUnfocus = optional(stack, unfocus);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            if (hasFocus) {
                window::focusSound(window) = focus;
            }
            if (hasUnfocus) {
                window::unfocusSound(window) = unfocus;
            }
        }
    }

    void nativeSetVisibilitySounds(UObject* object, Frame& stack, void*)
    {
        UObject* visibleSound = nullptr;
        UObject* invisibleSound = nullptr;
        bool hasVisible = optional(stack, visibleSound);
        bool hasInvisible = optional(stack, invisibleSound);
        vm::finish(stack);
        if (UWindow* window = self(object)) {
            if (hasVisible) {
                window::visibleSound(window) = visibleSound;
            }
            if (hasInvisible) {
                window::invisibleSound(window) = invisibleSound;
            }
        }
    }
}

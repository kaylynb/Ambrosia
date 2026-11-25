#pragma once

#include "extension/windows.h"

// Helpers shared by the files that make up the window classes.
namespace dx::extension {
    void runVoid(vm::Call& call);
    UWindow* windowOf(UObject* object);
    bool attachedTo(UWindow* window, UWindow* owner);
    UWindow* nextChild(UWindow* owner, UWindow* child, UWindow* following, UWindow* previous);
    void drawRoot(Canvas2D& canvas);
}

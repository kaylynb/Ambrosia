#pragma once

#include "debugui/overlay.h"

namespace dx::debugui {
    // The Memory tool: objects by class, the object and name tables, what the garbage collector
    // has done (at level changes and during play, by class), a row per level change, and the
    // KeepFreedObjects switch.
    void addMemoryTool(Overlay& overlay);
}

#pragma once

#include "debugui/overlay.h"

namespace dx::debugui {
    // The Game debug tool: the original game's debugging commands (the actor display, its tool
    // windows, window statistics and the player cheats) as controls that send them.
    void addGameDebugTool(Overlay& overlay);
}

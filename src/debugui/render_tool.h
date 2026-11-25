#pragma once

#include "debugui/overlay.h"

namespace dx::debugui {
    // The Render tool: switches that leave out or simplify render stages (dx::RenderDebug), for
    // bisecting render cost on slow hardware, and what the renderer did in the last frame. Each
    // switch is also "DebugOverlay render-<stage> on|off", and "DebugOverlay render-reset now"
    // turns every stage back on.
    void addRenderTool(Overlay& overlay);
}

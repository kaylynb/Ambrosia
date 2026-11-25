#pragma once

#include "debugui/overlay.h"

namespace dx::debugui {
    // The overlay's section for the raytracing sound mode: the scene Steam Audio traces against,
    // drawn over the level by material.
    void addAcousticsSection(Overlay& overlay);
}

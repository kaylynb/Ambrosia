#pragma once

#include "debugui/overlay.h"

namespace dx::debugui {
    // The overlay's audio section: the HRTF and reverb switches, the playing voices with solo
    // and mute, the zone's reverb, the mixer's cost and where the listener is.
    void addAudioSection(Overlay& overlay);
}

#pragma once

#include "engine/gyro.h"

#include <optional>

namespace dx::game::switchgyro {
    // The Switch's own gyro, read through libnx because the SDL3 fork has no controller
    // sensors: the console's in handheld mode, else player one's Pro Controller or right Joy-Con.
    // Does nothing elsewhere (game/desktop/switch_gyro.cc).
    void start();
    void stop();

    // The average of the samples since the last call, in SDL's gamepad convention: the rate in
    // radians a second and the accelerometer's reading (pointing up at rest), in g.
    struct Sample {
        gyro::Turn rate;
        Vector accelerometer;
    };

    // Nothing when there are no new samples.
    std::optional<Sample> poll();
}

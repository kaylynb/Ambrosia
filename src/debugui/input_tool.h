#pragma once

#include "debugui/overlay.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dx::debugui {
    // What the game knows about its input devices, for the Input tool; the tool reads each pad's
    // live buttons, axes and sensors from SDL itself.
    struct InputState {
        struct Pad {
            SDL_Gamepad* pad = nullptr;
            // The prompt style chosen for it from what SDL says it is.
            std::string promptStyle;
            // The pad in play, and the pad the prompts follow.
            bool inPlay = false;
            bool prompts = false;
        };

        struct Touch {
            int64_t id = 0;
            float x = 0;
            float y = 0;
        };

        std::vector<Pad> pads;
        // Whether the pad, rather than the keyboard and mouse, was used last.
        bool padLastUsed = false;
        // The prompt style being drawn, after the Auto setting.
        std::string shownStyle;
        bool gyroAiming = false;
        // The gyro rate last read from the pad in play, in radians a second.
        bool gyroRead = false;
        float gyroPitch = 0;
        float gyroYaw = 0;
        float gyroRoll = 0;
        // Down in the controller's axes, from its accelerometer; the drift taken off its gyro
        // (pitch, yaw, roll); and the calibration under way, as text.
        float down[3] = { 0, -1, 0 };
        float drift[3] = {};
        std::string calibration;
        // How long the gyro's turn is kept back to pay out evenly between bursts, in seconds.
        float gyroKeptBack = 0;
        // The fingers the platform layer reported this frame, in window pixels.
        std::vector<Touch> touches;
    };

    // The Input tool: each connected pad's name, type, prompt style and live state, the gyro and
    // the touch screen. read is called each frame the tool is drawn.
    void addInputTool(Overlay& overlay, std::function<InputState()> read);
}

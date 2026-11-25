#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace dx::game {
    // A finger on the touch screen, in window pixels.
    struct Finger {
        int64_t id = 0;
        float x = 0;
        float y = 0;
    };

    // Touch as the mouse in menus and other cursor screens. One finger drives the cursor: it
    // moves the cursor where it lands and presses the left button, a drag moves with the button
    // held (scroll bars, sliders, inventory items), and lifting it lets go, so a tap is a click.
    // Fingers that land while no screen wants the cursor, in play, are ignored until lifted.
    class TouchCursor {
    public:
        struct Host {
            std::function<bool()> wantsCursor;
            std::function<void(float x, float y)> move;
            std::function<void(bool pressed)> button;
            // After the release that ended a touch, where the cursor is now.
            std::function<void()> released;
        };

        explicit TouchCursor(Host callbacks);
        // The fingers down now, each frame.
        void update(std::span<const Finger> fingers);

        bool touching() const
        {
            return tracking.has_value();
        }

        // The fingers the last update was given, for the Input tool.
        const std::vector<Finger>& fingers() const
        {
            return lastFingers;
        }

    private:
        Host host;
        std::vector<Finger> lastFingers;
        std::optional<int64_t> tracking;
        float lastX = 0;
        float lastY = 0;
        std::set<int64_t> ignored;
    };
}

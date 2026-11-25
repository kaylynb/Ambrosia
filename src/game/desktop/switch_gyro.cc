#include "game/switch_gyro.h"

// SDL reads a desktop controller's gyro itself (gamepad_input.cc), so there is nothing here.
namespace dx::game::switchgyro {
    void start()
    {
    }

    void stop()
    {
    }

    std::optional<Sample> poll()
    {
        return std::nullopt;
    }
}

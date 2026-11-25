#pragma once

// The game started by the Ambrosia runner (src/switch-runner), which puts
// --ambrosia-runner <runner path> in front of the options. Shared by the Switch's platform files.
namespace dx::game::report {
    // From then on a fatal error is only logged, without the error screen that waits for a
    // button, so the game exits and the loader starts the runner again.
    void setUnattended();
}

#pragma once

// GPU time per stage of the frame for the built-in frame recorder (core/profile.h), from OpenGL
// timestamp queries, in every build. Off until "DebugOverlay gpu-timers on"; off, a scope is a
// load and a branch. On, each scope writes a timestamp at its start and end, and after each
// swap the results of the frame issued FrameLag frames before are read if the GPU has finished
// them (a frame not yet finished is skipped, never waited for), so the plots trail the CPU's
// rows by that many frames:
//   GPU scene        the whole 3D scene: main view, sky, mirrors and post chain
//   GPU mirrors      the mirror views
//   GPU translucent  the translucent passes of every view, mirrors' included
//   GPU post chain   resolve, ambient occlusion and FXAA
//   GPU HUD          the HUD and console scripts' canvas drawing and its flush
//   GPU main view    GPU scene less mirrors and post chain
//   GPU timer frames late  frames whose results were not ready when read

#include <cstdint>

namespace dx::gputimers {
    enum class Stage : uint8_t {
        Scene,
        Mirrors,
        Translucent,
        PostChain,
        Hud,
        Count,
    };

    constexpr int FrameLag = 3;

    void setEnabled(bool on);
    bool enabled();

    // Times the GL commands issued until the end of the scope.
    class Scope {
    public:
        explicit Scope(Stage stage);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        int pair = -1;
    };

    // After the swap: reads the frame FrameLag frames back and plots it.
    void endFrame();
}

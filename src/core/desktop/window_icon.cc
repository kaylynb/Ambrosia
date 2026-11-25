#include "core/platform.h"
#include "core/log.h"
#include "core/sdl_handles.h"

#include <SDL3/SDL.h>

#include <span>
#include <string_view>
#include <utility>

namespace dx::assets {
    // assets/icon/<program>/<size>.png, embedded in each program by dx_program_icon
    // (src/CMakeLists.txt), so the game and the editor each have their own.
    std::span<const std::pair<std::string_view, std::span<const unsigned char>>> windowIcon();
}

namespace dx::platform {
    namespace {
        using sdl::Surface;

        Surface decode(std::string_view name)
        {
            for (const auto& [stem, bytes] : assets::windowIcon()) {
                if (stem == name) {
                    return Surface(SDL_LoadPNG_IO(SDL_IOFromConstMem(bytes.data(), bytes.size()), true));
                }
            }
            SDL_SetError("no embedded %.*s.png", static_cast<int>(name.size()), name.data());
            return Surface();
        }

        bool setIcon(SDL_Window* window)
        {
            Surface base = decode("32");
            if (!base) {
                return false;
            }
            for (std::string_view alternate : { "64", "128", "256" }) {
                // The base surface takes its own reference to each alternate.
                Surface image = decode(alternate);
                if (!image || !SDL_AddSurfaceAlternateImage(base.get(), image.get())) {
                    return false;
                }
            }
            // SDL copies the icon, so the surfaces can go once it is set.
            return SDL_SetWindowIcon(window, base.get());
        }
    }

    void setWindowIcon(SDL_Window* window)
    {
        if (!setIcon(window)) {
            dx::log::warn("Init", "the window keeps the system's icon: {}", SDL_GetError());
        }
    }
}

#pragma once

// Owners for the SDL objects the programs make and give back: each closes or frees what it holds
// when it goes. The deleters are empty types, so a handle is the size of the pointer it holds.

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_surface.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dx::sdl {
    struct DestroySurface {
        void operator()(SDL_Surface* surface) const
        {
            SDL_DestroySurface(surface);
        }
    };

    struct CloseGamepad {
        void operator()(SDL_Gamepad* gamepad) const
        {
            SDL_CloseGamepad(gamepad);
        }
    };

    // For what SDL hands back to be freed with SDL_free: strings, and arrays such as display
    // modes.
    struct Free {
        void operator()(void* memory) const
        {
            SDL_free(memory);
        }
    };

    using Surface = std::unique_ptr<SDL_Surface, DestroySurface>;
    using Gamepad = std::unique_ptr<SDL_Gamepad, CloseGamepad>;
    template <typename T>
    using Owned = std::unique_ptr<T, Free>;

    // The clipboard's text, empty when it has none or cannot be read.
    inline std::string clipboardText()
    {
        Owned<char> text(SDL_GetClipboardText());
        return text ? std::string(text.get()) : std::string();
    }

    // Writes RGBA pixels, rows top first, as a BMP; false with SDL's error set when it cannot.
    // The surface borrows the pixels, which SDL only reads. The file is opened here rather than
    // by SDL_SaveBMP, which (in SDL 3.4.20) loses the converted copy it has made of the surface
    // when the file cannot be made; SDL_SaveBMP_IO frees it, and closes the file either way.
    inline bool saveBmp(const std::string& path, std::vector<uint8_t>& rgba, int width, int height)
    {
        Surface surface(SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ABGR8888, rgba.data(), width * 4));
        if (!surface) {
            return false;
        }
        SDL_IOStream* file = SDL_IOFromFile(path.c_str(), "wb");
        return file && SDL_SaveBMP_IO(surface.get(), file, true);
    }

    // Writes BGRA pixels, rows top first, as a PNG with their alpha, the file opened here as for
    // saveBmp; false with SDL's error set when it cannot.
    inline bool savePng(const std::string& path, std::vector<uint8_t>& bgra, int width, int height)
    {
        Surface surface(SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_BGRA32, bgra.data(), width * 4));
        if (!surface) {
            return false;
        }
        SDL_IOStream* file = SDL_IOFromFile(path.c_str(), "wb");
        return file && SDL_SavePNG_IO(surface.get(), file, true);
    }
}

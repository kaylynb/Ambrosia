#include "game/game.h"

#include "core/log.h"
#include "core/sdl_handles.h"
#include "core/settings.h"
#include "engine/resolutions.h"
#include "game/platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace dx::game {
    // Records the mode the window is in, which can differ from the one asked for when the
    // system adjusts or refuses it.
    bool Game::isFullscreen() const
    {
        return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    }

    void Game::saveVideoMode()
    {
        SDL_SyncWindow(window);
        bool fullscreen = isFullscreen();
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSize(window, &width, &height) || width <= 0 || height <= 0) {
            dx::log::warn("Log", "cannot read the window size: {}", SDL_GetError());
            return;
        }
        appliedWindow = fullscreen ? appliedWindow : dx::resolutions::Size { width, height };
        dx::settings::set({
            dx::settings::to(fullscreen ? dx::settings::fullscreenViewportX : dx::settings::windowedViewportX, width),
            dx::settings::to(fullscreen ? dx::settings::fullscreenViewportY : dx::settings::windowedViewportY, height),
            dx::settings::to(dx::settings::startupFullscreen, fullscreen),
        });
    }

    // Puts the window in the middle of its display's usable area, frame included, keeping
    // the title bar on screen when the window is taller than the room.
    void Game::centreWindow()
    {
        SDL_Rect usable {};
        if (isFullscreen() || !SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable)) {
            return;
        }
        int top = 0;
        int left = 0;
        int bottom = 0;
        int right = 0;
        if (!SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right)) {
            top = left = bottom = right = 0;
        }
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);
        int x = usable.x + left + std::max(0, (usable.w - width - left - right) / 2);
        int y = usable.y + top + std::max(0, (usable.h - height - top - bottom) / 2);
        SDL_SetWindowPosition(window, x, y);
    }

    // A window resized by dragging its edge, or maximized, keeps that size next time, as
    // choosing it in the Screen Resolution menu does. Sizes the game set itself are
    // already saved, or are the first run's default and stay unsaved.
    void Game::windowResized(int width, int height)
    {
        if (options.hidden || options.automated() || isFullscreen()) {
            return;
        }
        if (dx::resolutions::Size { width, height } == appliedWindow) {
            resizeSaveAt = 0;
            return;
        }
        // Dragging sends a stream of sizes; save once it settles.
        resizeSaveAt = SDL_GetPerformanceCounter() + SDL_GetPerformanceFrequency() / 2;
    }

    void Game::saveSettledResize()
    {
        if (resizeSaveAt != 0 && SDL_GetPerformanceCounter() >= resizeSaveAt) {
            resizeSaveAt = 0;
            if (!isFullscreen()) {
                saveVideoMode();
            }
        }
    }

    // Fullscreen is borderless at the desktop's resolution, as modern games do, rather than
    // switching the display's mode. ExclusiveFullscreen=True in the ini switches it as Deus
    // Ex did, to the closest mode to FullscreenViewportX/Y.
    bool Game::exclusiveFullscreen()
    {
        return dx::settings::get(dx::settings::exclusiveFullscreen);
    }

    dx::resolutions::Size Game::desktopSize()
    {
        const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window));
        return mode ? dx::resolutions::Size { mode->w, mode->h } : dx::resolutions::Size { 1920, 1080 };
    }

    // The largest window that fits on the display: its usable area less the window's frame.
    dx::resolutions::Size Game::windowRoom()
    {
        SDL_Rect usable {};
        if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable)) {
            dx::resolutions::Size desktop = desktopSize();
            usable = { 0, 0, desktop.width, desktop.height };
        }
        int top = 0;
        int left = 0;
        int bottom = 0;
        int right = 0;
        if (isFullscreen() || !SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right)) {
            // The frame cannot be measured: no system reports it for a fullscreen or hidden
            // window, and Wayland reports neither it nor the panels or docks, so the usable
            // area is the whole display. Keep an eighth of each side clear for a title bar
            // and panels on any edge, or the default would be a window the display's size,
            // which GNOME leaves taller than the screen and stops drawing when it comes out
            // of fullscreen. Erring small is fine: the player can pick a larger size.
            left = right = usable.w / 16;
            top = bottom = usable.h / 16;
        }
        return { usable.w - left - right, usable.h - top - bottom };
    }

    // The windowed size from the ini. 640x480 is what Deus Ex's ini ships with rather than
    // a choice, so it counts as unset and the largest standard size that fits is used.
    dx::resolutions::Size Game::windowedSize()
    {
        dx::resolutions::Size size { dx::settings::get(dx::settings::windowedViewportX), dx::settings::get(dx::settings::windowedViewportY) };
        if (size.width <= 0 || size.height <= 0 || size == dx::resolutions::Size { 640, 480 }) {
            return dx::resolutions::defaultWindow(desktopSize(), windowRoom());
        }
        return size;
    }

    // The exclusive fullscreen mode from the ini, or the desktop's size.
    dx::resolutions::Size Game::fullscreenSize()
    {
        dx::resolutions::Size size { dx::settings::get(dx::settings::fullscreenViewportX), dx::settings::get(dx::settings::fullscreenViewportY) };
        return exclusiveFullscreen() && size.width > 0 && size.height > 0 ? size : desktopSize();
    }

    void Game::applyVideoMode(bool fullscreen, int width, int height)
    {
        if (platform::setVideoMode(window, fullscreen, fullscreen && exclusiveFullscreen(), width, height) && !fullscreen) {
            appliedWindow = { width, height };
        }
    }

    // What the Screen Resolution choice offers, for the mode the window is in now.
    std::string Game::availableResolutions()
    {
        dx::resolutions::Size current;
        SDL_GetWindowSize(window, &current.width, &current.height);
        dx::resolutions::Size native = desktopSize();
        if (!isFullscreen()) {
            return dx::resolutions::format(dx::resolutions::windowed(native, windowRoom(), current));
        }
        if (!exclusiveFullscreen()) {
            return dx::resolutions::format({ native });
        }
        std::vector<dx::resolutions::Size> modes;
        int count = 0;
        if (dx::sdl::Owned<SDL_DisplayMode*> list { SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window), &count) }) {
            for (int i = 0; i < count; i++) {
                modes.push_back({ list.get()[i]->w, list.get()[i]->h });
            }
        }
        return dx::resolutions::format(dx::resolutions::exclusive(modes, native, current));
    }
}

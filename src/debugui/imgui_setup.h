#pragma once

#include <SDL3/SDL.h>

#include <span>

// Dear ImGui as the editor and the game's debug overlay both set it up: no imgui.ini, the dark
// style and the default font, on SDL3 and OpenGL 3.3. Fonts merged into the default one are
// added after startImGui and before the first frame.
namespace dx::debugui {
    void startImGui(SDL_Window* window, SDL_GLContext context);
    void stopImGui();

    // The scale that suits the window: its display's content scale, or, where the desktop
    // claims none (a 4K screen at 100%), the window's height over 1080 pixels in quarter steps.
    float autoScale(SDL_Window* window);
    // Draws ImGui at this scale: the dark style's sizes and the font, both times scale. Only
    // between frames; the same scale again does nothing.
    void setScale(float scale);
    float scale();
    // A size in pixels at scale 1, at the current scale, for fixed widths and window sizes.
    float scaled(float size);

    // A dim "(?)" on the same line as the last item, showing text when hovered: for
    // explanations that would otherwise crowd the window.
    void helpMarker(const char* text);
    // The same text as a tooltip on the last item, when hovered.
    void itemHelp(const char* text);

    // The scales offered in the menus, besides Auto.
    std::span<const float> scaleChoices();
}

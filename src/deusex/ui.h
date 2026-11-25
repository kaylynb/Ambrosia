#pragma once

#include "core/object.h"
#include "engine/canvas.h"

#include <cstdint>
#include <vector>

namespace dx {
    class UFont;
    class UTexture;
}

namespace dx::ui {
    // The colors of the theme the player chose in the game's options, so overlays drawn
    // natively match the screens around them. The game keeps one theme for the HUD and persona
    // screens and another for the main menus.
    struct Theme {
        // Panels behind native overlays, a dimmed shade of the theme background.
        LinearColor panel;
        LinearColor border;
        LinearColor text;
        LinearColor highlight;
    };

    Theme theme(UObject* player, bool menu = false);
    // The same active/inactive icon colors as the keyboard HUD's augmentation bar.
    LinearColor augmentationColor(bool active);
    // Whether a window belongs to the main menus rather than the HUD.
    bool isMenuWindow(UObject* window);

    // A font from the game's packages by path, such as "DeusExUI.FontMenuSmall", loaded once.
    // Falls back to the extension's own font if the game's is missing.
    UFont* font(const char* path);
    // A transient texture from BGRA pixels, row by row.
    UTexture* newTexture(uint32_t width, uint32_t height, std::vector<uint8_t> pixels, UObject* outer = nullptr);
    // A rectangle's outline, drawn inside it.
    void drawOutline(Canvas2D& canvas, float x, float y, float width, float height, float thickness, LinearColor color);
    LinearColor scaled(LinearColor color, float amount);
}

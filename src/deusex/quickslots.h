#pragma once

#include "core/object.h"
#include "engine/canvas.h"

#include <array>
#include <string>

namespace dx::quickslots {
    // The four d-pad directions, clockwise from up like the radial slices.
    enum Slot : int {
        Up,
        Right,
        Down,
        Left,
        Count,
    };

    // What each slot holds: "item:<class>" or "aug:<class>", or empty. Assigned from the
    // radial menus and kept with the game being played, so each savegame has its own.
    std::array<std::string, Count> assigned();
    // Puts an item or aug on a slot, taking it off any other slot it was on.
    void assign(Slot slot, const std::string& id);
    // Discards invalid slots on load; known inventory items need not be in stock.
    void prune(UObject* player);
    // Uses what the slot holds: an item goes in hand as from the belt, an aug toggles.
    void use(UObject* player, Slot slot);
    // Shared by the d-pad and belt wheel. Empty for items without ammo or a stack.
    std::string itemBadge(UObject* item);
    void drawBadge(Canvas2D& canvas, const std::string& badge, float right, float bottom, float width, float height, LinearColor color);
    // The d-pad cross on the HUD that stands in for the belt and aug bar with a pad.
    void draw(Canvas2D& canvas, UObject* player);
    // The top edge of that cross, for placing things above it.
    float top(const Canvas2D& canvas);
}

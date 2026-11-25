#pragma once

#include "core/object.h"
#include "engine/canvas.h"

#include <string>
#include <vector>

namespace dx {
    class UTexture;

    // Gamepad radial menus over the object belt and the augmentations.
    namespace radial {
        struct Entry {
            UTexture* icon = nullptr;
            // The full name, shown in the middle of the wheel.
            std::string label;
            // The two lines under the icon, as on the HUD belt: the loaded ammo or how many there
            // are, then the short belt name. Augmentations show their name on the second line.
            std::string detail;
            std::string badge;
            std::string shortName;
            // Console command that uses the entry; empty for an empty belt slot.
            std::string command;
            // "item:<class>" or "aug:<class>", for putting the entry on a d-pad quick slot.
            std::string id;
            // The number or key the game shows for it, such as a belt slot or F3.
            std::string key;
            // Augmentations toggle on and off; inactive ones are drawn dimmed.
            bool toggle = false;
            bool active = false;
        };

        // The ten belt slots in key order 1..9 then 0, empty ones included so each slot keeps
        // its place on the wheel.
        std::vector<Entry> beltEntries(UObject* player);
        // Whether the weapon in hand has another compatible ammo type in inventory.
        bool canSwitchAmmo(UObject* player);
        // Owned augmentations that have a hotkey, in hotkey order. Active ones are marked.
        std::vector<Entry> augEntries(UObject* player);
        // The slice a stick points at, slice 0 at the top and the rest clockwise, or -1 while
        // the stick is inside the dead zone. Screen-style y, down is positive.
        int pick(float x, float y, int count, float deadZone);
        // Drawn in the colors of the player's HUD theme, with a badge on entries that are on a
        // d-pad quick slot.
        void draw(Canvas2D& canvas, const std::vector<Entry>& entries, int selected, const std::string& title, UObject* player, bool switchAmmo = false);
        // Draws an inventory or augmentation icon as large as fits in a square box. The game's
        // icon textures are padded, so only the part the picture covers is scaled to the box.
        // Augmentation art needs Translucent (black is transparent); items use Masked.
        void drawIcon(Canvas2D& canvas, UTexture* icon, float centerX, float centerY, float box, LinearColor tint, BlendStyle style = BlendStyle::Masked);
    }
}

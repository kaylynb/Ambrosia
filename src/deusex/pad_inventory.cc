#include "deusex/pad_inventory.h"

#include "core/vm.h"
#include "deusex/hud.h"
#include "deusex/script.h"
#include "deusex/ui.h"
#include "core/gc.h"
#include "engine/input.h"
#include "engine/resources.h"
#include "extension/extension.h"
#include "extension/windows.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dx::padinventory {
    namespace {
        using extension::Area;
        using gamepad::Action;
        using gamepad::triggers;

        bool inventoryScreen(UObject* top)
        {
            return script::isA(top, "PersonaScreenInventory");
        }

        // Whether focus is on an item in the inventory grid.
        bool onItem(UObject* top, UObject* focus)
        {
            return inventoryScreen(top) && script::isA(focus, "PersonaInventoryItemButton");
        }

        bool clearableSlot(UObject* slot)
        {
            UObject* item = script::object(slot, "item");
            return item && script::integer(slot, "objectNum") > 0 && !script::isA(item, "NanoKeyRing");
        }

        // An inventory item held with the pad. It is steered over the grid and the belt by
        // driving the screen's own drag code, as the mouse does, so dropping it re-arranges the
        // grid, assigns a belt slot or applies a mod or ammo to a weapon exactly as a drag and
        // drop would.
        struct Move {
            UObject* screen = nullptr;
            UObject* button = nullptr;
            int width = 1;
            int height = 1;
            int homeX = 0;
            int homeY = 0;
            int cellX = 0;
            int cellY = 0;
            // -1 while over the grid, otherwise an index into beltSlots().
            int belt = -1;
            // The message box asking whether to apply a weapon mod, while it is open.
            UObject* confirm = nullptr;
            // A mouse drag held back for that question: the mouse already aimed it, so a Yes
            // drops it where it is rather than at a pad target.
            bool fromMouse = false;
        };

        Move current;

        // Windows are deleted once their level is gone; nothing here may still point at one.
        gc::ReleaseHook releaseListener([](const gc::ReleasedSet& released) {
            if (released.count(current.screen) || released.count(current.button) || released.count(current.confirm)) {
                current = {};
            }
        });

        bool moving()
        {
            UObject* expectedTop = current.confirm ? current.confirm : current.screen;
            if (current.button && (current.button->destroyed || current.screen->destroyed || extension::topModal() != expectedTop)) {
                current = {};
            }
            return current.button != nullptr;
        }

        bool confirming()
        {
            return moving() && current.confirm;
        }

        // Takes hold of an item's button. Home is the cell the item still owns; a mouse drag
        // may already have slid its button elsewhere.
        void begin(UObject* screen, UObject* button, bool fromMouse)
        {
            UObject* item = script::object(button, "clientObject");
            current = {};
            current.screen = screen;
            current.button = button;
            current.width = std::max(1, script::integer(item, "invSlotsX"));
            current.height = std::max(1, script::integer(item, "invSlotsY"));
            current.homeX = current.cellX = script::integer(item, "invPosX");
            current.homeY = current.cellY = script::integer(item, "invPosY");
            current.fromMouse = fromMouse;
        }

        // The belt slots on the inventory screen, left to right as drawn.
        std::vector<std::pair<UObject*, Area>> beltSlots(UObject* screen)
        {
            UObject* belt = script::object(script::object(screen, "invBelt"), "objBelt");
            std::vector<std::pair<UObject*, Area>> slots;
            for (int32_t i = 0; belt && i < 10; i++) {
                UObject* slot = script::object(belt, "objects", i);
                if (std::optional<Area> area = extension::windowArea(slot)) {
                    slots.emplace_back(slot, *area);
                }
            }
            std::sort(slots.begin(), slots.end(), [](const auto& a, const auto& b) { return a.second.x < b.second.x; });
            return slots;
        }

        float centerX(const Area& area)
        {
            return area.x + area.width * 0.5f;
        }

        // The target area, in the root window's units.
        Area target()
        {
            if (current.belt >= 0) {
                auto slots = beltSlots(current.screen);
                return current.belt < static_cast<int>(slots.size()) ? slots[static_cast<size_t>(current.belt)].second : Area {};
            }
            Area items = extension::windowArea(script::object(current.screen, "winItems")).value_or(Area {});
            float cellWidth = static_cast<float>(script::integer(current.screen, "invButtonWidth"));
            float cellHeight = static_cast<float>(script::integer(current.screen, "invButtonHeight"));
            return { items.x + static_cast<float>(current.cellX) * cellWidth, items.y + static_cast<float>(current.cellY) * cellHeight, static_cast<float>(current.width) * cellWidth, static_cast<float>(current.height) * cellHeight };
        }

        // Points the screen's drag at the target. Twice, as two mouse moves would: the first
        // slides the button into an empty cell and the second finds it there, which is what
        // marks the cell as the drop spot.
        void aim()
        {
            Area to = target();
            Area screen = extension::windowArea(current.screen).value_or(Area {});
            float x = centerX(to) - screen.x;
            float y = to.y + to.height * 0.5f - screen.y;
            for (int i = 0; i < 2; i++) {
                vm::Call(current.screen, Name("UpdateDragMouse")).f(x).f(y).run();
            }
        }

        // The weapon the held mod would be applied to if dropped now, or null.
        UObject* modTarget()
        {
            UObject* mod = script::object(current.button, "clientObject");
            UObject* over = script::object(current.screen, "lastDragOverButton");
            UObject* weapon = script::isA(over, "PersonaInventoryItemButton") ? script::object(over, "clientObject") : nullptr;
            if (!script::isA(mod, "WeaponMod") || !script::isA(weapon, "DeusExWeapon")) {
                return nullptr;
            }
            return vm::Call(mod, Name("CanUpgradeWeapon")).obj(weapon).resultBool() ? weapon : nullptr;
        }

        // Weapon mods are used up when applied, so ask first with the game's own message box.
        bool askToApplyMod()
        {
            UObject* weapon = modTarget();
            UObject* root = extension::rootWindow();
            if (!weapon || !root) {
                return false;
            }
            std::string text = "Apply the " + script::string(script::object(current.button, "clientObject"), "ItemName") + " to the " + script::string(weapon, "ItemName") + "? The modification is used up.";
            current.confirm = vm::Call(root, Name("MessageBox")).s("Apply Modification").s(text).i(0).b(false).obj(nullptr).result<UObject*>();
            // The dragged button draws the mod as the mouse cursor while it is dragging, which
            // would sit over the question. The drop itself runs from the screen's own drag
            // state, so the button can stop drawing it.
            if (current.confirm) {
                script::setFlag(current.button, "bDragging", false);
            }
            return current.confirm != nullptr;
        }

        void finish(bool place, bool confirmed = false)
        {
            if (!place) {
                current.belt = -1;
                current.cellX = current.homeX;
                current.cellY = current.homeY;
            }
            // Aim again before dropping: a button slid into an empty cell only lands there on
            // the next layout pass, and the drop needs to find it under the target.
            if (!(place && current.fromMouse)) {
                aim();
            }
            if (place && !confirmed && askToApplyMod()) {
                return;
            }
            UObject* screen = current.screen;
            UObject* button = current.button;
            current = {};
            vm::Call(button, Name("FinishButtonDrag")).run();
            // Follow the item the screen selects afterwards, such as the weapon a mod went
            // into, since the mod's own button is gone by then.
            extension::setFocusWindow(script::object(screen, "selectedItem"));
        }

        // Closes the mod question and applies the mod, or puts it back where it came from.
        void answerMod(bool apply)
        {
            current.confirm = nullptr;
            if (UObject* root = extension::rootWindow()) {
                vm::Call(root, Name("PopWindow")).run();
            }
            finish(apply, true);
        }

        void steerBy(int dx, int dy)
        {
            UObject* player = script::object(current.screen, "player");
            int columns = std::max(1, script::integer(player, "maxInvCols"));
            int rows = std::max(1, script::integer(player, "maxInvRows"));
            auto slots = beltSlots(current.screen);
            float fromX = centerX(target());
            if (current.belt < 0) {
                current.cellX = std::clamp(current.cellX + dx, 0, std::max(0, columns - current.width));
                if (dy > 0 && current.cellY + current.height >= rows && !slots.empty()) {
                    // Off the bottom of the grid onto the belt slot below.
                    auto nearest = std::min_element(slots.begin(), slots.end(), [fromX](const auto& a, const auto& b) {
                        return std::abs(centerX(a.second) - fromX) < std::abs(centerX(b.second) - fromX);
                    });
                    current.belt = static_cast<int>(nearest - slots.begin());
                } else {
                    current.cellY = std::clamp(current.cellY + dy, 0, std::max(0, rows - current.height));
                }
            } else if (dy < 0) {
                // Back up into the bottom row of the grid.
                Area items = extension::windowArea(script::object(current.screen, "winItems")).value_or(Area {});
                float cellWidth = static_cast<float>(std::max(1, script::integer(current.screen, "invButtonWidth")));
                current.cellX = std::clamp(static_cast<int>(std::lround((fromX - items.x) / cellWidth - static_cast<float>(current.width) * 0.5f)), 0, std::max(0, columns - current.width));
                current.cellY = std::max(0, rows - current.height);
                current.belt = -1;
            } else {
                current.belt = std::clamp(current.belt + dx, 0, static_cast<int>(slots.size()) - 1);
            }
            aim();
        }

        // Repacks the inventory grid: guns largest first, then grenades, then everything else,
        // each placed by the game's own slot search. Should that order not fit where the items
        // fitted before, it tries largest first regardless of kind, and failing that leaves the
        // grid as it was.
        void sort(UObject* screen)
        {
            UObject* player = script::object(screen, "player");
            if (!player) {
                return;
            }

            struct Item {
                UObject* object;
                int width;
                int height;
                int homeX;
                int homeY;
                int kind;
            };

            std::vector<Item> items;
            for (UObject* item : script::inventory(player)) {
                if (!script::flag(item, "bDisplayableInv")) {
                    continue;
                }
                int kind = script::isGrenade(item) ? 1 : (script::isA(item, "DeusExWeapon") ? 0 : 2);
                items.push_back({ item, std::max(1, script::integer(item, "invSlotsX")), std::max(1, script::integer(item, "invSlotsY")), script::integer(item, "invPosX"), script::integer(item, "invPosY"), kind });
            }
            auto bySize = [](const Item& a, const Item& b) {
                if (a.width * a.height != b.width * b.height) {
                    return a.width * a.height > b.width * b.height;
                }
                if (a.width != b.width) {
                    return a.width > b.width;
                }
                return a.object->cls->name.str() < b.object->cls->name.str();
            };
            auto place = [player](const std::vector<Item>& order) {
                vm::Call(player, Name("ClearInventorySlots")).run();
                for (const Item& item : order) {
                    vm::Call find(player, Name("FindInventorySlotXY"));
                    if (!find.i(item.width).i(item.height).i(0).i(0).resultBool()) {
                        return false;
                    }
                    int32_t x = *reinterpret_cast<int32_t*>(find.parameter(2));
                    int32_t y = *reinterpret_cast<int32_t*>(find.parameter(3));
                    vm::Call(player, Name("PlaceItemInSlot")).obj(item.object).i(x).i(y).run();
                }
                return true;
            };
            std::vector<Item> order = items;
            std::stable_sort(order.begin(), order.end(), [&](const Item& a, const Item& b) {
                return a.kind != b.kind ? a.kind < b.kind : bySize(a, b);
            });
            bool sorted = place(order);
            if (!sorted) {
                std::stable_sort(order.begin(), order.end(), bySize);
                sorted = place(order);
            }
            if (!sorted) {
                vm::Call(player, Name("ClearInventorySlots")).run();
                for (const Item& item : items) {
                    vm::Call(player, Name("PlaceItemInSlot")).obj(item.object).i(item.homeX).i(item.homeY).run();
                }
            }
            // The screen rebuilds its item buttons, so keep focus on the same item's new one.
            UObject* focused = script::object(extension::focusWindow(), "clientObject");
            vm::Call(screen, Name("RefreshInventoryItemButtons")).run();
            auto* grid = cast<extension::UWindow>(script::object(screen, "winItems"));
            for (extension::UWindow* button = grid ? grid->firstChild() : nullptr; button && focused; button = button->nextSibling()) {
                if (script::object(button, "clientObject") == focused) {
                    extension::setFocusWindow(button);
                    break;
                }
            }
        }
    }

    bool press(UObject* top, gamepad::Button button)
    {
        if (confirming()) {
            // MenuSelect answers with the focused button, Yes unless navigation moved to No.
            if (triggers(Action::MenuSelect, button)) {
                answerMod(extension::focusWindow() != script::object(current.confirm, "btnNo"));
            } else if (triggers(Action::MenuBack, button)) {
                answerMod(false);
            }
            return !triggers(Action::MenuNavigate, button);
        }
        if (moving()) {
            // While an item is held only placing it or putting it back does anything. The
            // d-pad is left to the caller, which steers the item with it as with the stick.
            bool place = triggers(Action::PlaceItem, button);
            if (place || triggers(Action::MenuBack, button)) {
                finish(place);
            }
            return !triggers(Action::MenuNavigate, button);
        }
        if (!inventoryScreen(top)) {
            return false;
        }
        if (triggers(Action::Sort, button)) {
            sort(top);
            return true;
        }
        UObject* focus = extension::focusWindow();
        if (beltFocused(top)) {
            if (triggers(Action::ClearSlot, button)) {
                if (clearableSlot(focus)) {
                    vm::Call(script::object(top, "invBelt"), Name("RemoveObject")).obj(script::object(focus, "item")).run();
                }
                return true;
            }
            // Do not apply grid shortcuts to an old selection while browsing the quickbar.
            return triggers(Action::Equip, button) || triggers(Action::Drop, button) || triggers(Action::ChangeAmmo, button);
        }
        if (triggers(Action::Equip, button) || triggers(Action::Drop, button)) {
            // These shortcuts act on the highlighted item, not a previously selected one.
            // Selecting only when the action is pressed leaves A's select-then-use intact.
            if (onItem(top, focus)) {
                vm::Call(top, Name("SelectInventory")).obj(focus).run();
            }
            script::pressButton(top, triggers(Action::Equip, button) ? "btnEquip" : "btnDrop");
            return true;
        }
        if (!onItem(top, focus)) {
            return false;
        }
        if (triggers(Action::MoveItem, button) && script::object(focus, "clientObject") && script::flag(focus, "bAllowDragging")) {
            begin(top, focus, false);
            vm::Call(focus, Name("StartButtonDrag")).run();
            aim();
            return true;
        }
        // MenuSelect selects an item, and on the item already selected uses it.
        return triggers(Action::MenuSelect, button) && usesOnA(top) && script::pressButton(top, "btnUse");
    }

    bool busy()
    {
        return moving();
    }

    std::vector<prompts::Prompt> busyPrompts()
    {
        if (confirming()) {
            return { { Action::MenuSelect, "Choose" }, { Action::MenuBack, "Cancel" } };
        }
        return { { Action::MenuNavigate, "Move" }, { Action::PlaceItem, "Place" }, { Action::MenuBack, "Cancel" } };
    }

    bool usesOnA(UObject* top)
    {
        UObject* focus = extension::focusWindow();
        return onItem(top, focus) && script::flag(focus, "bSelected") && script::usableButton(top, "btnUse");
    }

    bool beltFocused(UObject* top)
    {
        UObject* focus = extension::focusWindow();
        return inventoryScreen(top) && script::isA(focus, "HUDObjectSlot") && script::object(focus, "winInv") == top;
    }

    void addPrompts(UObject* top, std::vector<prompts::Prompt>& out)
    {
        if (!inventoryScreen(top)) {
            return;
        }
        UObject* focus = extension::focusWindow();
        if (beltFocused(top)) {
            out.push_back({ Action::MenuNavigate, "Move" });
            if (clearableSlot(focus)) {
                out.push_back({ Action::ClearSlot, "Clear slot" });
            }
            return;
        }
        if (onItem(top, focus) && script::flag(focus, "bAllowDragging")) {
            out.push_back({ Action::MoveItem, "Move" });
        }
        // Predict the buttons for the highlighted item without selecting it just to draw the
        // prompts. These are the same equip/drop restrictions as the screen's EnableButtons.
        UObject* target = onItem(top, focus) ? focus : script::object(top, "selectedItem");
        UObject* item = script::object(target, "clientObject");
        if (script::isA(item, "Inventory") && !script::isA(item, "NanoKeyRing")) {
            if (!script::isA(item, "Ammo")) {
                UObject* player = script::object(top, "player");
                bool equipped = item == script::object(player, "inHand") || item == script::object(player, "inHandPending");
                out.push_back({ Action::Equip, equipped ? "Unequip" : "Equip" });
            }
            out.push_back({ Action::Drop, "Drop" });
        }
        out.push_back({ Action::Sort, "Sort" });
    }

    bool steer(uint8_t key)
    {
        if (confirming()) {
            return false;
        }
        if (!moving()) {
            UObject* top = extension::topModal();
            if (inventoryScreen(top)) {
                // The game's quickbar slots opt out of keyboard focus. Include empty slots
                // too, so clearing one doesn't strand the gamepad cursor or skip a position.
                for (const auto& slot : beltSlots(top)) {
                    vm::Call(slot.first, Name("SetSelectability")).b(true).run();
                }
            }
            return false;
        }
        steerBy(key == IK_Left ? -1 : (key == IK_Right ? 1 : 0), key == IK_Up ? -1 : (key == IK_Down ? 1 : 0));
        return true;
    }

    bool buttonPressed(UObject* button)
    {
        if (!confirming()) {
            return false;
        }
        UObject* yes = script::object(current.confirm, "btnYes");
        if (button == yes || button == script::object(current.confirm, "btnNo")) {
            answerMod(button == yes);
            return true;
        }
        return false;
    }

    bool keyPressed(uint8_t key)
    {
        if (!confirming()) {
            return false;
        }
        if (key == IK_Enter || key == IK_Space || key == IK_A + ('Y' - 'A')) {
            answerMod(true);
            return true;
        }
        if (key == IK_Escape || key == IK_A + ('N' - 'A')) {
            answerMod(false);
            return true;
        }
        return false;
    }

    bool mouseReleased(UObject* window, uint8_t button)
    {
        UObject* top = extension::topModal();
        if (button != IK_LeftMouse || moving() || !inventoryScreen(top) || !script::isA(window, "PersonaInventoryItemButton") || !script::flag(window, "bDragging")) {
            return false;
        }
        // Take over the drag the mouse started, so the question and its answer work as for a
        // pad move.
        begin(top, window, true);
        if (askToApplyMod()) {
            return true;
        }
        current = {};
        return false;
    }

    void drawOverlay(Canvas2D& canvas)
    {
        // A held-back mouse drop has no pad target to show, and nothing is drawn over the mod
        // question.
        if (!moving() || current.fromMouse || confirming()) {
            return;
        }
        Area area = target();
        float scale = extension::uiScale();
        float x = area.x * scale;
        float y = area.y * scale;
        float w = area.width * scale;
        float h = area.height * scale;
        float line = std::max(2.0f, std::round(scale * 1.5f));
        ui::drawOutline(canvas, x - line, y - line, w + line * 2, h + line * 2, line, ui::theme(hud::viewer()).highlight);
        // The item itself, nudged up and left of the target so the target's own drop color
        // shows beside it, as the icon trails the cursor in a mouse drag.
        UObject* button = current.button;
        if (auto* icon = cast<UTexture>(script::object(button, "icon"))) {
            float iconWidth = static_cast<float>(script::integer(button, "iconPosWidth"));
            float iconHeight = static_cast<float>(script::integer(button, "iconPosHeight"));
            float nudge = std::round(std::min(w, h) * 0.2f);
            float iconX = std::round(x + (w - iconWidth * scale) * 0.5f - nudge);
            float iconY = std::round(y + (h - iconHeight * scale) * 0.5f - nudge);
            canvas.drawTile(icon, iconX, iconY, iconWidth * scale, iconHeight * scale, 0, 0, iconWidth, iconHeight, {}, BlendStyle::Masked, false);
        }
    }
}

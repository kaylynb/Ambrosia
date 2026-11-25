#include "deusex/quickslots.h"

#include "core/objects.h"
#include "core/vm.h"
#include "deusex/game_state.h"
#include "deusex/glyphs.h"
#include "deusex/radial.h"
#include "deusex/script.h"
#include "deusex/ui.h"
#include "engine/console.h"
#include "core/package.h"
#include "engine/props_engine.h"
#include "engine/resources.h"

#include <algorithm>
#include <cmath>

namespace dx::quickslots {
    namespace {
        // The slots belong to the game being played: they are kept in its state, as
        // "Up=item:WeaponPistol" lines, and saved and loaded with it.
        constexpr const char* section = "QuickSlots";
        constexpr std::array<const char*, Count> keys { "Up", "Right", "Down", "Left" };

        std::string slotId(int slot)
        {
            return gamestate::get(section, keys[static_cast<size_t>(slot)]);
        }

        // Splits "item:WeaponPistol" into its kind and class name.
        bool parse(const std::string& id, std::string& kind, std::string& className)
        {
            size_t colon = id.find(':');
            if (colon == std::string::npos) {
                return false;
            }
            kind = id.substr(0, colon);
            className = id.substr(colon + 1);
            return (kind == "item" || kind == "aug") && !className.empty();
        }

        // The player's item or installed aug that an id names, or null.
        UObject* find(UObject* player, const std::string& id)
        {
            std::string kind;
            std::string className;
            if (!parse(id, kind, className)) {
                return nullptr;
            }
            for (UObject* object : kind == "aug" ? script::augmentations(player) : script::inventory(player)) {
                if (iequals(object->cls->name.str(), className)) {
                    return object;
                }
            }
            return nullptr;
        }

        UObject* itemDefaults(const std::string& id)
        {
            if (!id.starts_with("item:")) {
                return nullptr;
            }
            UClass* cls = objects::findClass("DeusEx." + id.substr(5));
            return cls && script::isA(cls->defaultObject, "Inventory") ? cls->defaultObject : nullptr;
        }

        struct Shown {
            UTexture* icon = nullptr;
            std::string badge;
            bool active = false;
            BlendStyle style = BlendStyle::Masked;
        };

        Shown shown(UObject* player, const std::string& id)
        {
            Shown out;
            UObject* source = find(player, id);
            if (!source) {
                source = itemDefaults(id);
                if (!source) {
                    return out;
                }
                out.badge = "0";
            } else {
                out.badge = itemBadge(source);
            }
            bool aug = id.starts_with("aug:");
            out.style = aug ? BlendStyle::Translucent : BlendStyle::Masked;
            out.active = aug && script::flag(source, "bIsActive");
            UObject* icon = script::object(source, aug ? "smallIcon" : "Icon");
            objects::ensureLoaded(icon);
            out.icon = cast<UTexture>(icon);
            return out;
        }
    }

    std::string itemBadge(UObject* item)
    {
        if (script::isGrenade(item)) {
            return std::to_string(script::integer(script::object(item, "AmmoType"), "AmmoAmount"));
        }
        if (script::isA(item, "DeusExWeapon") && script::integer(item, "ReloadCount") > 0 && !script::flag(item, "bHandToHand")) {
            if (vm::stateName(item) == Name("Reload")) {
                auto* hud = objects::findClass("DeusEx.HUDAmmoDisplay");
                return script::string(hud ? hud->defaultObject : nullptr, "msgReloading");
            }
            if (script::integer(item, "ReloadCount") > 1) {
                int rounds = vm::Call(item, Name("AmmoLeftInClip")).result<int32_t>();
                int spares = vm::Call(item, Name("NumClips")).result<int32_t>();
                return std::to_string(rounds) + "/" + std::to_string(spares);
            }
            return std::to_string(script::integer(script::object(item, "AmmoType"), "AmmoAmount"));
        }
        if (script::isA(item, "Ammo")) {
            return std::to_string(script::integer(item, "AmmoAmount"));
        }
        if (script::flag(item, "bCanHaveMultipleCopies") && !script::isA(item, "NanoKeyRing")) {
            return std::to_string(script::integer(item, "NumCopies"));
        }
        return {};
    }

    void drawBadge(Canvas2D& canvas, const std::string& badge, float right, float bottom, float width, float height, LinearColor color)
    {
        if (!badge.empty()) {
            if (UFont* font = ui::font("DeusExUI.FontMenuSmall")) {
                float scale = std::min(height / std::max(1.0f, textHeight(font)), width / std::max(1.0f, textWidth(font, badge, 0)));
                drawText(canvas, font, badge, right - textWidth(font, badge, 0, scale), bottom - textHeight(font, scale), color, BlendStyle::Masked, 0, scale);
            }
        }
    }

    std::array<std::string, Count> assigned()
    {
        std::array<std::string, Count> ids;
        for (int i = 0; i < Count; i++) {
            ids[static_cast<size_t>(i)] = slotId(i);
        }
        return ids;
    }

    void assign(Slot slot, const std::string& id)
    {
        for (int i = 0; i < Count; i++) {
            if (i == slot || (!id.empty() && slotId(i) == id)) {
                gamestate::set(section, keys[static_cast<size_t>(i)], i == slot ? id : std::string());
            }
        }
    }

    void prune(UObject* player)
    {
        for (int i = 0; player && i < Count; i++) {
            std::string id = slotId(i);
            if (!id.empty() && !find(player, id) && !itemDefaults(id)) {
                gamestate::set(section, keys[static_cast<size_t>(i)], {});
            }
        }
    }

    void use(UObject* player, Slot slot)
    {
        std::string id = slotId(slot);
        UObject* object = find(player, id);
        if (!object) {
            return;
        }
        if (id.starts_with("aug:")) {
            console::execute(player, "ActivateAugmentation " + std::to_string(script::integer(object, "HotKeyNum") - 3));
            return;
        }
        // As DeusExRootWindow.ActivateObjectInBelt: ammo loads into the gun in hand, anything
        // else goes in hand.
        UObject* weapon = pawn::Weapon(player);
        if (script::isA(object, "Ammo") && weapon) {
            vm::Call(weapon, Name("LoadAmmoType")).obj(object).run();
        } else {
            vm::Call(player, Name("PutInHand")).obj(object).run();
        }
    }

    namespace {
        struct Layout {
            float cell;
            float gap;
            float centerX;
            float centerY;
        };

        Layout layout(const Canvas2D& canvas)
        {
            float cell = std::round(canvas.height() * 0.07f);
            float gap = std::round(cell * 0.08f);
            return { cell, gap, std::round(canvas.width() - cell * 2.0f - gap * 4.0f), std::round(canvas.height() - cell * 2.0f - gap * 4.0f) };
        }

        void drawBox(Canvas2D& canvas, float x, float y, float size, LinearColor panel, LinearColor edge)
        {
            canvas.drawRect(x, y, size, size, panel, BlendStyle::Normal);
            ui::drawOutline(canvas, x, y, size, size, 1, edge);
        }
    }

    float top(const Canvas2D& canvas)
    {
        Layout l = layout(canvas);
        return l.centerY - l.cell * 1.5f - l.gap;
    }

    void draw(Canvas2D& canvas, UObject* player)
    {
        auto [cell, gap, centerX, centerY] = layout(canvas);
        ui::Theme theme = ui::theme(player);
        LinearColor panel = theme.panel;
        panel.a = 0.75f;
        constexpr std::array<std::pair<float, float>, Count> offsets { { { 0.0f, -1.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { -1.0f, 0.0f } } };
        for (int i = 0; i < Count; i++) {
            float x = centerX + offsets[static_cast<size_t>(i)].first * (cell + gap) - cell * 0.5f;
            float y = centerY + offsets[static_cast<size_t>(i)].second * (cell + gap) - cell * 0.5f;
            Shown slot = shown(player, slotId(i));
            drawBox(canvas, x, y, cell, panel, slot.active ? theme.highlight : theme.border);
            if (slot.icon) {
                LinearColor tint = slot.style == BlendStyle::Translucent ? ui::augmentationColor(slot.active) : LinearColor {};
                if (slot.badge == "0") {
                    tint = { 0.35f, 0.35f, 0.35f, 1 };
                }
                radial::drawIcon(canvas, slot.icon, x + cell * 0.5f, y + cell * 0.5f, cell - gap * 2, tint, slot.style);
            }
            drawBadge(canvas, slot.badge, x + cell - gap, y + cell - gap, cell - 2 * gap, cell * 0.32f, theme.text);
        }
        // One d-pad in the middle of the cross says what the four boxes answer to, kept within
        // the gap between the boxes.
        float glyphSize = std::max(16.0f, std::floor(cell * 0.6f / 16.0f) * 16.0f);
        glyphs::draw(canvas, glyphs::image(gamepad::Action::QuickSlots), centerX - glyphSize * 0.5f, centerY - glyphSize * 0.5f, glyphSize, theme.text);
        // The aug bar is hidden with a pad, so active augmentations show in a row running left
        // from the bottom of the cross, leaving the space above it to the controls list.
        float box = std::round(cell * 0.75f);
        float rowX = centerX - cell * 1.5f - gap * 4 - box;
        float rowY = centerY + cell * 1.5f + gap - box;
        for (const radial::Entry& aug : radial::augEntries(player)) {
            if (!aug.active || rowX < 0) {
                continue;
            }
            drawBox(canvas, rowX, rowY, box, panel, theme.highlight);
            radial::drawIcon(canvas, aug.icon, rowX + box * 0.5f, rowY + box * 0.5f, box - gap * 2, ui::augmentationColor(true), BlendStyle::Translucent);
            rowX -= box + gap;
        }
    }
}

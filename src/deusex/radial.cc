#include "deusex/radial.h"

#include "core/gc.h"
#include "core/objects.h"
#include "deusex/glyphs.h"
#include "deusex/quickslots.h"
#include "deusex/references.h"
#include "deusex/script.h"
#include "deusex/ui.h"
#include "engine/resources.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <functional>
#include <map>
#include <numbers>

namespace dx::radial {
    namespace {
        constexpr float tau = 2.0f * std::numbers::pi_v<float>;

        UTexture* texture(UObject* object, const char* name)
        {
            UObject* value = script::object(object, name);
            objects::ensureLoaded(value);
            return cast<UTexture>(value);
        }

        // An opaque gray texture whose brightness at each pixel comes from shade, which gets
        // coordinates in -1..1 across the texture. Drawn modulated it darkens, drawn translucent
        // it adds light, so shapes such as the ring need no alpha.
        UTexture* shadeTexture(int size, const std::function<float(float x, float y)>& shade)
        {
            std::vector<uint8_t> pixels(static_cast<size_t>(size * size) * 4);
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size; x++) {
                    float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                    float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                    auto value = static_cast<uint8_t>(std::clamp(shade(u, v), 0.0f, 1.0f) * 255.0f + 0.5f);
                    uint8_t* pixel = &pixels[static_cast<size_t>(y * size + x) * 4];
                    pixel[0] = value;
                    pixel[1] = value;
                    pixel[2] = value;
                    pixel[3] = 255;
                }
            }
            return ui::newTexture(static_cast<uint32_t>(size), static_cast<uint32_t>(size), std::move(pixels));
        }

        constexpr int textureSize = 256;
        // A pixel of the texture in radius units, for soft one-pixel edges.
        constexpr float pixel = 2.0f / static_cast<float>(textureSize);

        float edge(float distance)
        {
            return std::clamp(0.5f - distance / pixel, 0.0f, 1.0f);
        }

        // The ring holding the slices, between inner and 1, with a darker hub. Drawn modulated,
        // which doubles: mid gray leaves the world as it is and darker grays darken it.
        UTexture* discTexture(float inner)
        {
            static deusex::CachedObjects<std::map<float, UTexture*>> cache;
            UTexture*& made = cache.entries[inner];
            if (!made) {
                made = shadeTexture(textureSize, [inner](float x, float y) {
                    float r = std::sqrt(x * x + y * y);
                    float ring = edge(r - 1.0f) * edge(inner - r);
                    float hub = edge(r - inner);
                    // The ring at 30% of the world's brightness, the hub at 15%, outside unchanged.
                    return 0.5f * (1.0f - ring * 0.7f - hub * 0.85f);
                });
            }
            return made;
        }

        // A thin circle at radius, for the ring's edges.
        UTexture* circleTexture(float radius)
        {
            static deusex::CachedObjects<std::map<float, UTexture*>> cache;
            UTexture*& made = cache.entries[radius];
            if (!made) {
                made = shadeTexture(textureSize, [radius](float x, float y) {
                    return edge(std::abs(std::sqrt(x * x + y * y) - radius) - pixel * 0.6f);
                });
            }
            return made;
        }

        // The highlighted slice: the part of the ring within half a slice of its center angle,
        // brightest at its outer edge.
        UTexture* wedgeTexture(int count, int index, float inner)
        {
            static deusex::CachedObjects<std::map<std::pair<int, int>, UTexture*>> cache;
            UTexture*& made = cache.entries[{ count, index }];
            if (!made) {
                float half = tau / static_cast<float>(count) * 0.5f;
                float center = tau / static_cast<float>(count) * static_cast<float>(index);
                made = shadeTexture(textureSize, [=](float x, float y) {
                    float r = std::sqrt(x * x + y * y);
                    float angle = std::atan2(x, -y) - center;
                    angle = std::remainder(angle, tau);
                    float sideDistance = (std::abs(angle) - half) * r;
                    float inside = edge(r - 1.0f) * edge(inner - r) * edge(sideDistance);
                    return inside * (0.45f + 0.55f * (r - inner) / (1.0f - inner));
                });
            }
            return made;
        }

        void drawText(Canvas2D& canvas, UFont* face, const std::string& line, float centerX, float y, LinearColor color, float scale = 1.0f)
        {
            if (face && !line.empty()) {
                dx::drawText(canvas, face, line, std::floor(centerX - textWidth(face, line, 0, scale) * 0.5f), std::floor(y), color, BlendStyle::Masked, 0, scale);
            }
        }

        // The line HUDObjectSlot.UpdateItemText shows above a belt item's name: the loaded
        // ammo for a gun, how many there are for grenades and stacked pickups.
        std::string beltDetail(UObject* item)
        {
            using script::integer;
            if (script::isA(item, "DeusExWeapon")) {
                UObject* ammoClass = script::object(item, "AmmoName");
                UObject* ammo = script::object(item, "AmmoType");
                std::string detail;
                if (ammoClass && ammoClass->name.str() != "AmmoNone" && !script::flag(item, "bHandToHand") && integer(item, "ReloadCount") != 0 && ammo) {
                    detail = script::string(ammo, "beltDescription");
                }
                if (script::isGrenade(item) && ammo && integer(ammo, "AmmoAmount") > 1) {
                    UClass* slot = objects::findClass("DeusEx.HUDObjectSlot");
                    detail = script::string(slot ? slot->defaultObject : nullptr, "CountLabel") + " " + std::to_string(integer(ammo, "AmmoAmount"));
                }
                return detail;
            }
            if (script::isA(item, "DeusExPickup") && !script::isA(item, "NanoKeyRing") && integer(item, "NumCopies") > 1) {
                return script::string(item, "CountLabel") + " " + std::to_string(integer(item, "NumCopies"));
            }
            return {};
        }

        using ui::scaled;
    }

    void drawIcon(Canvas2D& canvas, UTexture* icon, float centerX, float centerY, float box, LinearColor tint, BlendStyle style)
    {
        if (!icon || icon->mips.empty()) {
            return;
        }
        // Item icons mask palette index zero; augmentation art is translucent, where black
        // contributes nothing regardless of its palette index. Match that when cropping too.
        // Keyed on the icon without keeping it: an icon that goes takes its entry with it, so a
        // texture made later in the same memory is measured afresh.
        static std::map<std::pair<UTexture*, BlendStyle>, std::array<float, 4>> areas;
        static gc::ReleaseHook areasReleased([](const gc::ReleasedSet& released) {
            std::erase_if(areas, [&](const auto& entry) { return released.count(entry.first.first) != 0; });
        });
        auto key = std::make_pair(icon, style);
        auto found = areas.find(key);
        if (found == areas.end()) {
            const TextureMip& mip = icon->mips[0];
            int width = static_cast<int>(mip.width);
            int height = static_cast<int>(mip.height);
            uint8_t format = *icon->propertyAddress(Name("Format"));
            bool rgba = format == 5;
            size_t count = static_cast<size_t>(std::max(width, 0)) * static_cast<size_t>(std::max(height, 0));
            // Other formats, or a mip short of its size, go through the decoded pixels.
            bool direct = (format == 0 || rgba) && mip.data.size() >= count * (rgba ? 4 : 1);
            bool decoded = style == BlendStyle::Translucent || !direct;
            std::vector<uint8_t> pixels;
            if (decoded) {
                try {
                    pixels = icon->rgba(0);
                } catch (const std::exception&) {
                    // Draws the whole texture, uncropped.
                }
            }
            int left = width;
            int top = height;
            int right = -1;
            int bottom = -1;
            for (int y = 0; y < height && (!decoded || pixels.size() >= count * 4); y++) {
                for (int x = 0; x < width; x++) {
                    size_t index = static_cast<size_t>(y * width + x);
                    bool opaque = style == BlendStyle::Translucent
                        ? pixels[index * 4] != 0 || pixels[index * 4 + 1] != 0 || pixels[index * 4 + 2] != 0
                        : !direct ? pixels[index * 4 + 3] != 0
                                  : (rgba ? mip.data[index * 4 + 3] != 0 : mip.data[index] != 0);
                    if (opaque) {
                        left = std::min(left, x);
                        top = std::min(top, y);
                        right = std::max(right, x);
                        bottom = std::max(bottom, y);
                    }
                }
            }
            std::array<float, 4> area { 0, 0, static_cast<float>(width), static_cast<float>(height) };
            if (right >= left && bottom >= top) {
                area = { static_cast<float>(left), static_cast<float>(top), static_cast<float>(right - left + 1), static_cast<float>(bottom - top + 1) };
            }
            found = areas.emplace(key, area).first;
        }
        auto [u, v, w, h] = found->second;
        float scale = std::min(box / w, box / h);
        // Unfiltered, so the icons keep the same crisp pixels as on the game's own belt.
        canvas.drawTile(icon, std::round(centerX - w * scale * 0.5f), std::round(centerY - h * scale * 0.5f), std::round(w * scale), std::round(h * scale), u, v, w, h, tint, style, false);
    }

    std::vector<Entry> beltEntries(UObject* player)
    {
        std::vector<Entry> entries(10);
        for (int position = 0; position < 10; position++) {
            entries[static_cast<size_t>((position + 9) % 10)].key = std::to_string(position);
        }
        for (UObject* item : script::inventory(player)) {
            int32_t* position = script::field<int32_t>(item, "beltPos");
            if (!position || *position < 0 || *position > 9) {
                continue;
            }
            // Slot 1 sits at the top and slot 0 last, matching the number row.
            Entry& entry = entries[static_cast<size_t>((*position + 9) % 10)];
            entry.icon = texture(item, "Icon");
            entry.label = script::string(item, "ItemName");
            entry.detail = beltDetail(item);
            entry.badge = quickslots::itemBadge(item);
            entry.shortName = script::string(item, "beltDescription");
            entry.command = "ActivateBelt " + std::to_string(*position);
            entry.id = "item:" + item->cls->name.str();
        }
        return entries;
    }

    bool canSwitchAmmo(UObject* player)
    {
        UObject* weapon = script::object(player, "inHand");
        if (!script::isA(weapon, "DeusExWeapon")) {
            return false;
        }
        UObject* loaded = script::object(weapon, "AmmoName");
        for (int i = 0; i < 3; i++) {
            UObject* type = script::object(weapon, "AmmoNames", i);
            if (type && type != loaded) {
                for (UObject* item : script::inventory(player)) {
                    if (item->cls == type && script::integer(item, "AmmoAmount") > 0) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    std::vector<Entry> augEntries(UObject* player)
    {
        std::array<Entry, 10> byKey {};
        for (UObject* aug : script::augmentations(player)) {
            int32_t hotKey = script::integer(aug, "HotKeyNum");
            if (hotKey >= 3 && hotKey <= 12) {
                Entry& entry = byKey[static_cast<size_t>(hotKey - 3)];
                entry.icon = texture(aug, "smallIcon");
                entry.label = script::string(aug, "AugmentationName");
                // Augmentation names are long, so a two-word one takes both lines under the
                // icon, split at the space nearest its middle.
                entry.shortName = entry.label;
                size_t middle = entry.label.size() / 2;
                size_t before = entry.label.rfind(' ', middle);
                size_t after = entry.label.find(' ', middle);
                size_t split = before == std::string::npos ? after : (after == std::string::npos || middle - before <= after - middle ? before : after);
                if (entry.label.size() > 10 && split != std::string::npos) {
                    entry.detail = entry.label.substr(0, split);
                    entry.shortName = entry.label.substr(split + 1);
                }
                entry.command = "ActivateAugmentation " + std::to_string(hotKey - 3);
                entry.id = "aug:" + aug->cls->name.str();
                entry.key = "F" + std::to_string(hotKey);
                entry.toggle = true;
                entry.active = script::flag(aug, "bIsActive");
            }
        }
        std::vector<Entry> entries;
        for (Entry& entry : byKey) {
            if (!entry.command.empty()) {
                entries.push_back(std::move(entry));
            }
        }
        return entries;
    }

    int pick(float x, float y, int count, float deadZone)
    {
        if (count <= 0 || std::sqrt(x * x + y * y) < deadZone) {
            return -1;
        }
        float slice = tau / static_cast<float>(count);
        float angle = std::atan2(x, -y);
        if (angle < 0) {
            angle += tau;
        }
        return static_cast<int>(std::lround(angle / slice)) % count;
    }

    void draw(Canvas2D& canvas, const std::vector<Entry>& entries, int selected, const std::string& title, UObject* player, bool switchAmmo)
    {
        if (entries.empty()) {
            return;
        }
        ui::Theme theme = ui::theme(player);
        LinearColor border = theme.border;
        LinearColor highlight = theme.highlight;
        LinearColor textColor = theme.text;
        float centerX = canvas.width() * 0.5f;
        float centerY = canvas.height() * 0.5f;
        float outer = std::round(canvas.height() * 0.30f);
        constexpr float innerRatio = 0.48f;
        float inner = outer * innerRatio;
        float iconRing = (outer + inner) * 0.5f;
        float cell = (outer - inner) * 0.56f;
        int count = static_cast<int>(entries.size());
        float slice = tau / static_cast<float>(count);
        float left = centerX - outer;
        float top = centerY - outer;
        float diameter = outer * 2.0f;

        // Dim the world, then darken the ring and its hub further.
        canvas.drawRect(0, 0, canvas.width(), canvas.height(), { 0, 0, 0, 0.35f }, BlendStyle::Normal);
        canvas.drawTile(discTexture(innerRatio), left, top, diameter, diameter, 0, 0, textureSize, textureSize, {}, BlendStyle::Modulated, true);
        if (selected >= 0 && selected < count) {
            canvas.drawTile(wedgeTexture(count, selected, innerRatio), left, top, diameter, diameter, 0, 0, textureSize, textureSize, scaled(highlight, 0.28f), BlendStyle::Translucent, true);
        }
        canvas.drawTile(circleTexture(1.0f - pixel), left, top, diameter, diameter, 0, 0, textureSize, textureSize, scaled(border, 0.9f), BlendStyle::Translucent, true);
        canvas.drawTile(circleTexture(innerRatio), left, top, diameter, diameter, 0, 0, textureSize, textureSize, scaled(border, 0.7f), BlendStyle::Translucent, true);
        // Dividers between slices.
        for (int i = 0; i < count; i++) {
            float angle = slice * (static_cast<float>(i) + 0.5f);
            float dx = std::sin(angle);
            float dy = -std::cos(angle);
            canvas.drawLine(centerX + dx * inner, centerY + dy * inner, centerX + dx * outer, centerY + dy * outer, scaled(border, 0.45f));
        }

        UFont* small = ui::font("DeusExUI.FontMenuSmall");
        std::array<std::string, quickslots::Count> quick = quickslots::assigned();
        for (int i = 0; i < count; i++) {
            const Entry& entry = entries[static_cast<size_t>(i)];
            bool chosen = i == selected;
            float angle = slice * static_cast<float>(i);
            float x = centerX + std::sin(angle) * iconRing;
            float y = centerY - std::cos(angle) * iconRing;
            float box = cell * (chosen ? 1.15f : 1.0f);
            if (entry.icon) {
                LinearColor tint = entry.toggle ? ui::augmentationColor(entry.active) : LinearColor {};
                drawIcon(canvas, entry.icon, x, y, box, tint, entry.toggle ? BlendStyle::Translucent : BlendStyle::Masked);
                quickslots::drawBadge(canvas, entry.badge, x + box * 0.5f, y + box * 0.5f, box, cell * 0.22f, textColor);
            }
            for (int slot = 0; slot < quickslots::Count; slot++) {
                if (entry.id.empty() || quick[static_cast<size_t>(slot)] != entry.id) {
                    continue;
                }
                glyphs::draw(canvas, glyphs::image(gamepad::dpadClockwise(slot)), x - box * 0.5f, y - box * 0.5f, glyphs::size(cell * 0.2f), highlight);
            }
        }

        // One font pixel at a wheel radius of 180 pixels (600p).
        float scale = outer / 180.0f;
        float gap = 4 * scale;
        UFont* heading = ui::font("DeusExUI.FontMenuHeaders");
        UFont* name = ui::font("DeusExUI.FontMenuTitle");
        drawText(canvas, heading, title, centerX, centerY - inner * 0.82f, border, scale);
        if (switchAmmo && small && canSwitchAmmo(player)) {
            // This acts on the equipped weapon, independently of the highlighted belt slice.
            float hintHeight = textHeight(small, scale);
            float glyphSize = glyphs::size(hintHeight * 1.5f);
            float width = glyphSize + gap + textWidth(small, "Switch ammo", 0, scale);
            float x = std::round(centerX - width * 0.5f);
            float y = centerY + outer + glyphSize * 0.25f;
            glyphs::draw(canvas, glyphs::image(gamepad::Action::SwitchAmmo), x, y, glyphSize, textColor);
            dx::drawText(canvas, small, "Switch ammo", x + glyphSize + gap, std::round(y + (glyphSize - hintHeight) * 0.5f), textColor, BlendStyle::Masked, 0, scale);
        }
        if (selected < 0 || selected >= count) {
            return;
        }
        const Entry& entry = entries[static_cast<size_t>(selected)];
        if (entry.icon) {
            LinearColor tint = entry.toggle ? ui::augmentationColor(entry.active) : LinearColor {};
            drawIcon(canvas, entry.icon, centerX, centerY - inner * 0.32f, inner * 0.62f, tint, entry.toggle ? BlendStyle::Translucent : BlendStyle::Masked);
            quickslots::drawBadge(canvas, entry.badge, centerX + inner * 0.31f, centerY - inner * 0.01f, inner * 0.62f, cell * 0.22f, textColor);
        }
        std::string label = entry.command.empty() ? std::string("Empty") : entry.label;
        // Shrink a long name to fit across the hub rather than spill over the ring.
        float nameScale = name ? std::min(scale, inner * 1.7f / std::max(1.0f, textWidth(name, label, 0))) : scale;
        float nameY = centerY + inner * 0.08f;
        drawText(canvas, name, label, centerX, nameY, entry.command.empty() ? scaled(textColor, 0.5f) : highlight, nameScale);
        if (entry.toggle) {
            drawText(canvas, small, entry.active ? "Active" : "Inactive", centerX, nameY + textHeight(name, nameScale) + 2 * scale, scaled(textColor, 0.8f), scale);
        } else if (!entry.detail.empty()) {
            float detailScale = small ? std::min(scale, inner * 1.7f / std::max(1.0f, textWidth(small, entry.detail, 0))) : scale;
            drawText(canvas, small, entry.detail, centerX, nameY + textHeight(name, nameScale) + 2 * scale, scaled(textColor, 0.8f), detailScale);
        }
        if (!entry.id.empty() && small) {
            // RadialUse uses the entry and RadialAssign puts it on a quick slot; letting go only closes.
            std::array<std::pair<gamepad::Action, std::string>, 2> hints { { { gamepad::Action::RadialUse, entry.toggle ? "Toggle" : "Use" }, { gamepad::Action::RadialAssign, "Assign" } } };
            float hintHeight = textHeight(small, scale);
            float hintY = centerY + inner * 0.62f;
            float glyphSize = glyphs::size(hintHeight * 1.5f);
            float spacing = glyphSize * 0.75f;
            float width = -spacing;
            for (const auto& [action, hint] : hints) {
                width += glyphSize + gap + textWidth(small, hint, 0, scale) + spacing;
            }
            float hintX = std::round(centerX - width * 0.5f);
            for (const auto& [action, hint] : hints) {
                glyphs::draw(canvas, glyphs::image(action), hintX, hintY + hintHeight * 0.5f - glyphSize * 0.5f, glyphSize, textColor);
                hintX += glyphSize + gap;
                dx::drawText(canvas, small, hint, hintX, std::floor(hintY), scaled(textColor, 0.8f), BlendStyle::Masked, 0, scale);
                hintX += textWidth(small, hint, 0, scale) + spacing;
            }
        }
    }
}

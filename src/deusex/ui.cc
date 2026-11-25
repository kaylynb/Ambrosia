#include "deusex/ui.h"

#include "core/math.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/references.h"
#include "deusex/script.h"
#include "engine/resources.h"

#include <map>
#include <string>

namespace dx::ui {
    namespace {
        // Menu themes have no border color; their buttons are drawn in the face color.
        constexpr const char* hudNames[] = { "HUDColor_Background", "HUDColor_Borders", "HUDColor_ButtonTextNormal", "HUDColor_ButtonTextFocus" };
        constexpr const char* menuNames[] = { "MenuColor_Background", "MenuColor_ButtonFace", "MenuColor_ButtonTextNormal", "MenuColor_ButtonTextFocus" };

        constexpr Theme fallback {
            { 0.03f, 0.05f, 0.07f, 1 },
            { 0.45f, 0.55f, 0.65f, 1 },
            { 0.85f, 0.9f, 0.95f, 1 },
            { 1.0f, 0.85f, 0.4f, 1 },
        };
    }

    Theme theme(UObject* player, bool menu)
    {
        UObject* manager = script::object(player, "ThemeManager");
        UObject* current = manager ? vm::Call(manager, Name(menu ? "GetCurrentMenuColorTheme" : "GetCurrentHUDColorTheme")).result<UObject*>() : nullptr;
        if (!current) {
            return fallback;
        }
        const char* const* names = menu ? menuNames : hudNames;
        Theme out = fallback;
        LinearColor* roles[] = { &out.panel, &out.border, &out.text, &out.highlight };
        for (int i = 0; i < 4; i++) {
            vm::Call lookup(current, Name("GetColorFromName"));
            lookup.n(Name(names[i])).run();
            if (lookup.returnValue()) {
                Color color = *reinterpret_cast<Color*>(lookup.returnValue());
                *roles[i] = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1 };
            }
        }
        // Theme backgrounds are drawn translucent over the world; an opaque panel needs a much
        // darker shade to sit the same way.
        out.panel = scaled(out.panel, 0.2f);
        return out;
    }

    LinearColor augmentationColor(bool active)
    {
        UClass* cls = objects::findClass("DeusEx.HUDActiveAug");
        if (Color* color = script::field<Color>(cls ? cls->defaultObject : nullptr, active ? "colAugActive" : "colAugInactive")) {
            return { color->r / 255.0f, color->g / 255.0f, color->b / 255.0f, 1 };
        }
        return active ? LinearColor { 1, 1, 0, 1 } : LinearColor { 100.0f / 255.0f, 100.0f / 255.0f, 100.0f / 255.0f, 1 };
    }

    bool isMenuWindow(UObject* window)
    {
        return script::isA(window, "MenuUIWindow");
    }

    UFont* font(const char* path)
    {
        // Held strongly: the fonts are handed out for as long as the game runs.
        static deusex::CachedObjects<std::map<std::string, UFont*>> cache;
        auto& loaded = cache.entries;
        auto found = loaded.find(path);
        if (found != loaded.end()) {
            return found->second;
        }
        UObject* object = objects::loadObject(path, nullptr);
        objects::ensureLoaded(object);
        UFont* result = cast<UFont>(object);
        if (!result) {
            object = objects::loadObject("Extension.TechMedium", nullptr);
            objects::ensureLoaded(object);
            result = cast<UFont>(object);
        }
        loaded[path] = result;
        return result;
    }

    UTexture* newTexture(uint32_t width, uint32_t height, std::vector<uint8_t> pixels, UObject* outer)
    {
        auto* texture = cast<UTexture>(objects::newObject(objects::findClass("Engine.Texture"), outer ? outer : objects::packageObject(Name("Transient")), Name(), RF_Public | RF_Transient));
        // TEXF_RGBA8.
        *texture->propertyAddress(Name("Format")) = 5;
        TextureMip mip;
        mip.width = width;
        mip.height = height;
        mip.data = std::move(pixels);
        texture->mips.push_back(std::move(mip));
        return texture;
    }

    void drawOutline(Canvas2D& canvas, float x, float y, float width, float height, float thickness, LinearColor color)
    {
        canvas.drawRect(x, y, width, thickness, color, BlendStyle::Normal);
        canvas.drawRect(x, y + height - thickness, width, thickness, color, BlendStyle::Normal);
        canvas.drawRect(x, y + thickness, thickness, height - thickness * 2, color, BlendStyle::Normal);
        canvas.drawRect(x + width - thickness, y + thickness, thickness, height - thickness * 2, color, BlendStyle::Normal);
    }

    LinearColor scaled(LinearColor color, float amount)
    {
        return { color.r * amount, color.g * amount, color.b * amount, color.a };
    }
}

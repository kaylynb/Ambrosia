#include "extension/windows.h"

#include "audio/audio.h"
#include "core/objects.h"
#include "engine/props_engine.h"
#include "engine/resources.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace dx::extension {
    State& state()
    {
        static State instance;
        return instance;
    }

    float fittedHeight()
    {
        return std::min(state().viewportHeight, state().viewportWidth * 0.75f);
    }

    float virtualScale()
    {
        if (state().uiScale > 0) {
            return static_cast<float>(state().uiScale);
        }
        return std::max(std::floor(fittedHeight() / 600.0f), 1.0f);
    }

    float virtualHeight()
    {
        return std::ceil(state().viewportHeight / virtualScale());
    }

    float virtualWidth()
    {
        return std::round(std::ceil(fittedHeight() / virtualScale()) * 4.0f / 3.0f);
    }

    float extendedVirtualWidth()
    {
        float realWidth = std::ceil(state().viewportWidth / virtualScale());
        // Whole units: 768 high is 1365.33 wide at 16:9, and a fractional width puts the HUD
        // and anything centred in it off the pixel grid.
        return std::max(std::min(realWidth, std::ceil(std::ceil(fittedHeight() / virtualScale()) * 16.0f / 9.0f)), virtualWidth());
    }

    bool spansSafeArea(UWindow* window, float absoluteX)
    {
        URootWindow* rootWindow = window->root();
        if (!window->parent() || !rootWindow) {
            return false;
        }
        float safeWidth = extendedVirtualWidth();
        float safeLeft = rootWindow->usedX - std::round((safeWidth - rootWindow->width()) * 0.5f);
        return absoluteX <= safeLeft + 0.5f && absoluteX + window->width() >= safeLeft + safeWidth - 0.5f;
    }

    UClass* extensionClass(const char* path)
    {
        UClass* cls = objects::findClass(path);
        objects::flushLoads();
        return cls;
    }

    UGC* sharedGC()
    {
        State& s = state();
        if (!s.gc) {
            UClass* cls = extensionClass("Extension.GC");
            if (cls) {
                s.gc = cast<UGC>(objects::newObject(cls, objects::packageObject(Name("Transient")), Name("RootGC"), RF_Transient));
            }
        }
        return s.gc;
    }

    UObject* defaultFont()
    {
        UObject*& font = state().font;
        if (!font) {
            font = objects::loadObject("Extension.TechMedium", nullptr);
            objects::ensureLoaded(font);
        }
        return font;
    }

    bool optionalBool(Frame& stack, bool& out)
    {
        uint32_t value = 0;
        if (!optional(stack, value)) {
            return false;
        }
        out = value != 0;
        return true;
    }

    bool optionalBoolDefault(Frame& stack, bool fallback)
    {
        bool value = fallback;
        optionalBool(stack, value);
        return value;
    }

    LinearColor toLinear(const Color& color)
    {
        LinearColor result;
        result.r = static_cast<float>(color.r) / 255.0f;
        result.g = static_cast<float>(color.g) / 255.0f;
        result.b = static_cast<float>(color.b) / 255.0f;
        result.a = 1.0f;
        return result;
    }

    float textureWidth(UObject* texture)
    {
        UTexture* tex = cast<UTexture>(texture);
        if (!tex) {
            return 0;
        }
        objects::ensureLoaded(tex);
        return static_cast<float>(tex->width());
    }

    float textureHeight(UObject* texture)
    {
        UTexture* tex = cast<UTexture>(texture);
        if (!tex) {
            return 0;
        }
        objects::ensureLoaded(tex);
        return static_cast<float>(tex->height());
    }

    float fontHeight(UObject* font)
    {
        UFont* value = cast<UFont>(font);
        if (!value) {
            return 0;
        }
        objects::ensureLoaded(value);
        const FontCharacter* glyph = value->glyph(static_cast<uint8_t>('X'), nullptr);
        return glyph ? static_cast<float>(glyph->height) : 0;
    }

    // XGC::GetFontHeight: the taller of the normal and
    // bold fonts' space characters.
    float lineFontHeight(UObject* normalFont, UObject* boldFont)
    {
        float height = 0;
        for (UObject* font : { normalFont, boldFont }) {
            UFont* value = cast<UFont>(font);
            if (!value) {
                continue;
            }
            objects::ensureLoaded(value);
            const FontCharacter* glyph = value->glyph(static_cast<uint8_t>(' '), nullptr);
            if (glyph) {
                height = std::max(height, static_cast<float>(glyph->height));
            }
        }
        return height;
    }

    void playSound(UWindow* window, UObject* sound, float volume, float pitch)
    {
        if (!sound) {
            return;
        }
        UObject* player = window ? window->playerPawn() : nullptr;
        State& s = state();
        if (s.soundPlayer) {
            s.soundPlayer(sound, player, volume, pitch);
            return;
        }
        USound* value = cast<USound>(sound);
        if (!value) {
            return;
        }
        objects::ensureLoaded(value);
        Vector location;
        if (player && player->data) {
            location = actor::Location(player);
        }
        audio::playSound(value, player, 0, location, volume, 4096, pitch, false);
    }

    std::string formatNumber(const std::string& format, double value)
    {
        std::string out;
        size_t pos = 0;
        while (pos < format.size()) {
            char c = format[pos];
            if (c != '%') {
                out.push_back(c);
                pos++;
                continue;
            }
            if (pos + 1 < format.size() && format[pos + 1] == '%') {
                out.push_back('%');
                pos += 2;
                continue;
            }
            pos++;
            bool left = false;
            bool zero = false;
            while (pos < format.size() && (format[pos] == '-' || format[pos] == '0' || format[pos] == '+' || format[pos] == ' ')) {
                left = left || format[pos] == '-';
                zero = zero || format[pos] == '0';
                pos++;
            }
            int width = 0;
            while (pos < format.size() && std::isdigit(static_cast<unsigned char>(format[pos]))) {
                width = width * 10 + (format[pos] - '0');
                pos++;
            }
            int precision = -1;
            if (pos < format.size() && format[pos] == '.') {
                pos++;
                precision = 0;
                while (pos < format.size() && std::isdigit(static_cast<unsigned char>(format[pos]))) {
                    precision = precision * 10 + (format[pos] - '0');
                    pos++;
                }
            }
            char spec = pos < format.size() ? format[pos] : 'g';
            pos++;
            char buffer[128];
            int signedWidth = left ? -width : width;
            if (spec == 'd' || spec == 'i') {
                if (zero && !left) {
                    std::snprintf(buffer, sizeof(buffer), "%0*ld", width, std::lround(value));
                } else {
                    std::snprintf(buffer, sizeof(buffer), "%*ld", signedWidth, std::lround(value));
                }
            } else if (spec == 'f' || spec == 'F') {
                std::snprintf(buffer, sizeof(buffer), "%*.*f", signedWidth, precision < 0 ? 6 : precision, value);
            } else if (spec == 'e' || spec == 'E') {
                std::snprintf(buffer, sizeof(buffer), "%*.*e", signedWidth, precision < 0 ? 6 : precision, value);
            } else {
                std::snprintf(buffer, sizeof(buffer), "%*.*g", signedWidth, precision < 0 ? 6 : precision, value);
            }
            out += buffer;
        }
        return out;
    }
}

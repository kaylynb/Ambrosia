#include "deusex/glyphs.h"

#include "core/sdl_handles.h"
#include "deusex/references.h"
#include "deusex/ui.h"
#include "engine/resources.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <span>
#include <string>
#include <utility>

namespace dx::assets {
    std::span<const std::pair<std::string_view, std::span<const unsigned char>>> gamepadGlyphs();
}

namespace dx::glyphs {
    namespace {
        Style currentStyle = Style::Xbox;
        Style detectedStyle = Style::Xbox;

        // Each mip a box-filtered half of the one before, down to 1x1, so the glyphs stay
        // smooth when drawn well under their 128 pixels.
        void addMips(UTexture* texture)
        {
            while (texture->mips.back().width > 1 || texture->mips.back().height > 1) {
                const TextureMip& from = texture->mips.back();
                TextureMip mip;
                mip.width = std::max(1u, from.width / 2);
                mip.height = std::max(1u, from.height / 2);
                mip.data.resize(static_cast<size_t>(mip.width) * mip.height * 4);
                for (uint32_t y = 0; y < mip.height; y++) {
                    for (uint32_t x = 0; x < mip.width; x++) {
                        for (uint32_t c = 0; c < 4; c++) {
                            uint32_t sum = 0;
                            for (uint32_t dy = 0; dy < 2; dy++) {
                                for (uint32_t dx = 0; dx < 2; dx++) {
                                    uint32_t sx = std::min(from.width - 1, x * 2 + dx);
                                    uint32_t sy = std::min(from.height - 1, y * 2 + dy);
                                    sum += from.data[(static_cast<size_t>(sy) * from.width + sx) * 4 + c];
                                }
                            }
                            mip.data[(static_cast<size_t>(y) * mip.width + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
                        }
                    }
                }
                texture->mips.push_back(std::move(mip));
            }
        }

        // A glyph is white art to be tinted: its color is made pure white so only its alpha
        // counts, and blended edges take no dark fringe from transparent pixels.
        UTexture* decode(std::span<const unsigned char> png, bool glyph)
        {
            sdl::Surface surface;
            if (sdl::Surface loaded { SDL_LoadPNG_IO(SDL_IOFromConstMem(png.data(), png.size()), true) }) {
                surface.reset(SDL_ConvertSurface(loaded.get(), SDL_PIXELFORMAT_BGRA32));
            }
            if (!surface) {
                return nullptr;
            }
            std::vector<uint8_t> pixels;
            for (int y = 0; y < surface->h; y++) {
                const auto* row = static_cast<const uint8_t*>(surface->pixels) + static_cast<ptrdiff_t>(y) * surface->pitch;
                pixels.insert(pixels.end(), row, row + static_cast<ptrdiff_t>(surface->w) * 4);
            }
            if (glyph) {
                for (size_t i = 0; i < pixels.size(); i += 4) {
                    pixels[i] = pixels[i + 1] = pixels[i + 2] = 255;
                }
            }
            UTexture* texture = ui::newTexture(static_cast<uint32_t>(surface->w), static_cast<uint32_t>(surface->h), std::move(pixels));
            if (texture && glyph) {
                addMips(texture);
            }
            return texture;
        }
    }

    void setStyle(Style style)
    {
        currentStyle = style;
    }

    Style style()
    {
        return currentStyle;
    }

    void setPadStyle(Style style)
    {
        detectedStyle = style;
    }

    Style padStyle()
    {
        return detectedStyle;
    }

    UTexture* image(std::string_view name)
    {
        // The glyphs are decoded all together the first time one is asked for.
        static deusex::CachedObjects<std::map<std::string, UTexture*, std::less<>>> cache(true);
        auto& loaded = cache.entries;
        if (loaded.empty()) {
            for (const auto& [stem, png] : assets::gamepadGlyphs()) {
                loaded[std::string(stem)] = decode(png, !stem.ends_with("_layout"));
            }
        }
        // The PlayStation ("ps_") and Switch ("ns_") sets have their own art for every
        // control but the back paddles, which fall back to the shared "pad_" glyphs.
        if (currentStyle != Style::Xbox && name.starts_with("pad_")) {
            std::string styled = (currentStyle == Style::PlayStation ? "ps_" : "ns_") + std::string(name.substr(4));
            // The touchpad click does what View does (gamepad::fromSdl), so it stands for View:
            // a PS5 pad's Create/Share button is awkward to reach.
            if (currentStyle == Style::PlayStation && name == "pad_view") {
                styled = "ps_touchpad";
            }
            if (auto found = loaded.find(styled); found != loaded.end()) {
                return found->second;
            }
        }
        auto found = loaded.find(name);
        return found == loaded.end() ? nullptr : found->second;
    }

    UTexture* image(gamepad::Button button)
    {
        const char* name = gamepad::glyphName(button);
        return name ? image(name) : nullptr;
    }

    UTexture* image(gamepad::Control control)
    {
        const char* name = gamepad::glyphName(control);
        return name ? image(name) : nullptr;
    }

    UTexture* image(gamepad::Action action)
    {
        return image(gamepad::shownControl(action));
    }

    float size(float wanted)
    {
        return std::max(16.0f, std::round(wanted));
    }

    void draw(Canvas2D& canvas, UTexture* image, float x, float y, float size, LinearColor color)
    {
        if (image) {
            canvas.drawTile(image, std::round(x), std::round(y), size, size, 0, 0, static_cast<float>(image->width()), static_cast<float>(image->height()), color, BlendStyle::Alpha, true);
        }
    }
}

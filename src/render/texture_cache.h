#pragma once

#include "core/gc.h"
#include "engine/resources.h"
#include "render/procedural.h"
#include "render/render_device.h"
#include "render/texture_replacement.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dx {
    struct TextureAnimation {
        UTexture* current = nullptr;
        double lastUpdate = 0;
        float accumulator = 0;
        int32_t primeCurrent = 0;
    };

    // Every texture pointer here is weak: an entry goes when its texture is collected. Its GL
    // texture is deleted at the next beginFrame rather than at once, because a collection (or a
    // clear) may come while a canvas batch or ImGui draw list drawn with it is still waiting to
    // be flushed.
    class TextureCache : private gc::Holder {
    public:
        static UTexture* advance(UTexture* head, TextureAnimation& animation, double now, float constantRate = 0.0f);

        TextureCache();
        ~TextureCache();
        TextureCache(const TextureCache&) = delete;
        TextureCache& operator=(const TextureCache&) = delete;

        void beginFrame(double time);
        UTexture* animated(UTexture* texture);
        const GLTextureInfo* get(UTexture* texture);
        const GLTextureInfo* getMasked(UTexture* texture);
        uint32_t white();
        void clear();
        void resetProcedural(UTexture* texture);
        void setFiltering(float newAnisotropy, bool newTrilinear);
        void setConstantRate(float rate);
        // Goes up each time an entry is dropped because its texture was collected. Whoever keeps
        // a GLTextureInfo pointer or a GL texture id past one frame compares this to know that
        // what it kept may be gone.
        uint64_t generation() const;
        // Whether any entry is kept for this object, compared by address alone.
        bool holds(const UObject* texture) const;

    private:
        bool upload(UTexture* texture, GLTextureInfo& info, bool masked);
        bool uploadReplacement(UTexture* texture, GLTextureInfo& info);
        void objectsReleased(const gc::ReleasedSet& released) override;
        // Takes the entry's texture to be deleted at the next beginFrame, with the rest at once.
        void retire(GLTextureInfo& info);
        void deleteRetired();

        // Names given up by their entries' handles, so they go in one glDeleteTextures.
        std::vector<uint32_t> retired;
        uint64_t releases = 0;

        std::unordered_map<UTexture*, GLTextureInfo> textures;
        std::unordered_map<UTexture*, GLTextureInfo> maskedTextures;
        std::unordered_map<UTexture*, TextureAnimation> animations;
        std::unordered_map<UTexture*, uint64_t> proceduralFrames;
        ProceduralTextures procedural;
        TextureReplacements replacements;
        TextureHandle whiteTexture;
        double time = 0;
        uint64_t frame = 0;
        float anisotropy = 1;
        float maxAnisotropy = 1;
        bool trilinear = false;
        float constantRate = 0;
    };
}

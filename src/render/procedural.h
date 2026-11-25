#pragma once

#include "core/gc.h"
#include "engine/resources.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace dx {
    struct ProceduralState;

    // Updates per second for textures the original game advanced once per frame.
    // Used when steady frame timing is enabled.
    constexpr float steadyTextureRate = 60.0f;
    // The most steps one frame catches up on; a longer stall drops the rest.
    constexpr int steadyCatchUp = 8;

    class ProceduralTextures {
    public:
        ProceduralTextures();
        ~ProceduralTextures();
        ProceduralTextures(const ProceduralTextures&) = delete;
        ProceduralTextures& operator=(const ProceduralTextures&) = delete;

        static bool isProcedural(UTexture* texture);
        bool update(UTexture* texture, double time, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
        void clear();
        void reset(UTexture* texture);
        // A rate above 0 steps textures without a frame rate limit at that fixed rate.
        void setConstantRate(float rate);
        // Drops the state of a texture that is collected, or whose palette is. Its owner, the
        // texture cache, passes this on.
        void objectsReleased(const gc::ReleasedSet& released);
        bool holds(const UObject* object) const;

    private:
        std::unordered_map<UTexture*, std::unique_ptr<ProceduralState>> states;
        float constantRate = 0;
    };
}

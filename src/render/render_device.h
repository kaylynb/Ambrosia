#pragma once

#include "render/gl_handle.h"
#include "render/gl_math.h"

#include <cstdint>
#include <vector>

namespace dx {
    struct GLVertex {
        float x = 0;
        float y = 0;
        float z = 0;
        float u = 0;
        float v = 0;
        float lu = 0;
        float lv = 0;
        float r = 1;
        float g = 1;
        float b = 1;
        float a = 1;
        float nx = 0;
        float ny = 0;
        float nz = 1;
        float fogR = 0;
        float fogG = 0;
        float fogB = 0;
    };

    struct DynamicLight {
        Vector location;
        float radius = 0;
        Vector color;
        Vector direction;
        float cone = 2;
        bool specialLit = false;
    };

    // A texture the cache made, which deletes it (TextureCache); others borrow the name with id().
    struct GLTextureInfo {
        TextureHandle texture;
        uint32_t width = 1;
        uint32_t height = 1;
        bool masked = false;
        bool procedural = false;
        bool smooth = false;
        int32_t levels = 1;

        uint32_t id() const
        {
            return texture.get();
        }
    };

    constexpr int32_t SHADER_Textured = 1;
    constexpr int32_t SHADER_Lightmap = 2;
    constexpr int32_t SHADER_AlphaTest = 4;
    constexpr int32_t SHADER_Dynamic = 16;
    constexpr int32_t SHADER_Detail = 32;
    constexpr int32_t SHADER_Fog = 64;
    constexpr int32_t SHADER_Fullbright = 128;
    constexpr size_t MaxDynamicLights = 8;

    enum class BlendMode : uint8_t {
        Opaque,
        Masked,
        Translucent,
        Modulated,
        Alpha,
        Highlight,
    };

    enum class Primitive : uint8_t {
        Triangles,
        Lines,
    };

    enum class DepthFunc : uint8_t {
        LessEqual,
        Equal,
        Always,
    };

    enum class OcclusionQuality : uint8_t {
        Off,
        Low,
        Medium,
        High,
    };

    enum class OcclusionSource : uint8_t {
        None,
        World,
        Mesh,
    };
}

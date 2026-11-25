#pragma once

#include "core/objects.h"
#include "engine/props_engine.h"
#include "render/gl_device.h"
#include "render/lighting.h"
#include "render/render_debug.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Helpers shared by the files that implement SceneRenderer. They keep internal linkage, as
// they had when they all lived in scene_renderer.cc.
namespace dx {
    namespace {
        constexpr float sceneNearPlane = 4.0f;
        constexpr float sceneFarPlane = 65536.0f;
        constexpr float overlayNearPlane = 1.0f;
        const Name nameActor("Actor");
        const Name nameDecal("Decal");
        const Name nameLevelInfo("LevelInfo");
        const Name nameMover("Mover");
        const Name namePawn("Pawn");
        const Name namePlayerPawn("PlayerPawn");
        const Name nameSkyZoneInfo("SkyZoneInfo");
        const Name nameZoneInfo("ZoneInfo");

        constexpr uint32_t PF_Invisible = 0x00000001;
        constexpr uint32_t PF_Masked = 0x00000002;
        constexpr uint32_t PF_Translucent = 0x00000004;
        constexpr uint32_t PF_Environment = 0x00000010;
        constexpr uint32_t PF_Modulated = 0x00000040;
        constexpr uint32_t PF_FakeBackdrop = 0x00000080;
        constexpr uint32_t PF_TwoSided = 0x00000100;
        constexpr uint32_t PF_AutoUPan = 0x00000200;
        constexpr uint32_t PF_AutoVPan = 0x00000400;
        constexpr uint32_t PF_SmallWavy = 0x00002000;
        constexpr uint32_t PF_SpecialLit = 0x00100000;
        constexpr uint32_t PF_Unlit = 0x00400000;
        constexpr uint32_t PF_Portal = 0x04000000;
        constexpr uint32_t PF_Mirrored = 0x08000000;
        constexpr int32_t MaxFrameRecursion = 4;

        constexpr uint8_t DT_Sprite = 1;
        constexpr uint8_t DT_Mesh = 2;
        constexpr uint8_t DT_Brush = 3;
        constexpr uint8_t DT_SpriteAnimOnce = 7;

        constexpr uint8_t STY_None = 0;
        constexpr uint8_t STY_Masked = 2;
        constexpr uint8_t STY_Translucent = 3;
        constexpr uint8_t STY_Modulated = 4;

        inline bool isTranslucentFlags(uint32_t flags)
        {
            return (flags & (PF_Translucent | PF_Modulated)) != 0;
        }

        inline uint32_t styleFlags(UObject* actor)
        {
            switch (actor::Style(actor)) {
            case STY_Masked:
                return PF_Masked;
            case STY_Translucent:
                return PF_Translucent;
            case STY_Modulated:
                return PF_Modulated;
            default:
                return 0;
            }
        }

        inline uint32_t textureFlags(UTexture* texture)
        {
            if (!texture || !texture->data) {
                return 0;
            }
            uint32_t flags = 0;
            if (texture::bMasked(texture)) {
                flags |= PF_Masked;
            }
            if (texture::bTransparent(texture)) {
                flags |= PF_Translucent;
            }
            if (texture::bModulate(texture)) {
                flags |= PF_Modulated;
            }
            if (texture::bUnlit(texture)) {
                flags |= PF_Unlit;
            }
            if (texture::bTwoSided(texture)) {
                flags |= PF_TwoSided;
            }
            return flags;
        }

        inline UTexture* asTexture(UObject* object)
        {
            if (!object) {
                return nullptr;
            }
            objects::ensureLoaded(object);
            return cast<UTexture>(object);
        }

        inline void boundingSphere(const std::vector<Vector>& points, Vector& center, float& radius)
        {
            if (points.empty()) {
                center = {};
                radius = 0;
                return;
            }
            Vector low = points[0];
            Vector high = points[0];
            for (const Vector& point : points) {
                low = { std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z) };
                high = { std::max(high.x, point.x), std::max(high.y, point.y), std::max(high.z, point.z) };
            }
            center = (low + high) * 0.5f;
            float radiusSquared = 0;
            for (const Vector& point : points) {
                radiusSquared = std::max(radiusSquared, (point - center).sizeSquared());
            }
            radius = std::sqrt(radiusSquared) + 1.0f;
        }

        inline bool sameVector(const Vector& a, const Vector& b)
        {
            return a.x == b.x && a.y == b.y && a.z == b.z;
        }

        inline bool timeDependentEffect(uint8_t effect)
        {
            return effect == 5 || effect == 6;
        }

        inline Vector scaleOf(uint8_t* scaleStruct)
        {
            Vector value = *reinterpret_cast<Vector*>(scaleStruct);
            if (value.x == 0 && value.y == 0 && value.z == 0) {
                return { 1, 1, 1 };
            }
            return value;
        }
    }
}

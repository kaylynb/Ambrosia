#include "render/scene_renderer.h"

#include "core/objects.h"
#include "core/profile.h"
#include "engine/props_engine.h"
#include "engine/world.h"
#include "render/scene_renderer_internal.h"

#include <algorithm>
#include <cmath>

namespace dx {
    UTexture* spriteAnimOnceFrame(UObject* actor, UTexture* texture)
    {
        int32_t count = 1;
        for (UTexture* test = asTexture(texture::AnimNext(texture)); test && test != texture; test = asTexture(texture::AnimNext(test))) {
            count++;
        }
        double ratio = 1.0 - static_cast<double>(actor::LifeSpan(actor)) / static_cast<double>(actor::LifeSpan(actor->cls->defaultObject));
        double fraction = ratio < 0.0 ? 0.0 : (ratio < 1.0 ? ratio : 1.0);
        float scaled = static_cast<float>(fraction * static_cast<double>(count));
        int32_t frame = static_cast<int32_t>(std::nearbyint(scaled - 0.5f));
        frame = frame < 0 ? 0 : std::min(frame, count - 1);
        while (frame-- > 0) {
            texture = asTexture(texture::AnimNext(texture));
        }
        return texture;
    }

    bool drawnAsSprite(uint8_t drawType)
    {
        return drawType == DT_Sprite || drawType == DT_SpriteAnimOnce;
    }

    void SceneRenderer::drawSprite(UObject* actor, const View& view)
    {
        UTexture* texture = asTexture(actor::Texture(actor));
        if (!texture) {
            return;
        }
        texture = actor::DrawType(actor) == DT_SpriteAnimOnce ? spriteAnimOnceFrame(actor, texture) : device->animated(texture);
        if (!texture || actor::Style(actor) == STY_None) {
            return;
        }
        UTexture* matrix = matrixTexture(sceneWorld ? sceneWorld->player() : nullptr);
        UTexture* drawn = matrix ? device->animated(matrix) : texture;
        uint32_t flags = styleFlags(actor) | (textureFlags(drawn) & (PF_Masked | PF_Translucent | PF_Modulated));
        const GLTextureInfo* info = device->texture(drawn);
        if (!info) {
            return;
        }
        float drawScale = actor::DrawScale(actor);
        float halfWidth = static_cast<float>(texture->width()) * drawScale * 0.5f;
        float halfHeight = static_cast<float>(texture->height()) * drawScale * 0.5f;
        Vector center = actor::Location(actor) + actor::PrePivot(actor);
        Vector right = view.axes.yAxis;
        Vector up = view.axes.zAxis;
        float glow = std::min(actor::ScaleGlow(actor), 1.0f);
        Vector corners[4] = {
            center - right * halfWidth + up * halfHeight,
            center + right * halfWidth + up * halfHeight,
            center + right * halfWidth - up * halfHeight,
            center - right * halfWidth - up * halfHeight,
        };
        float us[4] = { 0, 1, 1, 0 };
        float vs[4] = { 0, 0, 1, 1 };
        std::vector<GLVertex> vertices;
        const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int index : order) {
            GLVertex vertex;
            vertex.x = corners[index].x;
            vertex.y = corners[index].y;
            vertex.z = corners[index].z;
            vertex.u = us[index];
            vertex.v = vs[index];
            vertex.r = glow;
            vertex.g = glow;
            vertex.b = glow;
            vertices.push_back(vertex);
        }
        if (isTranslucentFlags(flags)) {
            queueTranslucent(flags & (PF_Translucent | PF_Modulated | PF_Masked), info->id(), {}, vertices);
            return;
        }
        device->setOcclusionSource(OcclusionSource::None);
        applyBlend(flags, 0);
        device->setCulling(false);
        device->bindTexture(info->id());
        device->draw(vertices, Primitive::Triangles);
        device->setBlend(BlendMode::Opaque);
    }

    void SceneRenderer::drawActor(UObject* actor, const View& view)
    {
        if (actor->isA(nameDecal)) {
            drawDecal(actor, view);
            return;
        }
        switch (actor::DrawType(actor)) {
        case DT_Mesh:
            if (renderDebug.actorMeshes) {
                drawMesh(actor, view);
            }
            break;
        case DT_Brush:
            if (actor->isA(nameMover)) {
                drawBrush(actor, view);
            }
            break;
        default:
            if (drawnAsSprite(actor::DrawType(actor))) {
                drawSprite(actor, view);
            }
            break;
        }
    }

    void SceneRenderer::drawActors(const std::vector<UObject*>& actors, const View& view, UObject* viewActor, bool occlude)
    {
        device->useShader();
        device->setViewProj(view.viewProj);
        device->setPan(0, 0);
        device->setTint(1, 1, 1, 1);
        device->setDepthTest(true);
        device->setDepthFunc(DepthFunc::LessEqual);
        device->setFrontFace(!view.mirrored);
        bool behindView = false;
        if (viewActor && viewActor->data && viewActor->isA(namePawn)) {
            behindView = pawn::bBehindView(viewActor) != 0;
        }
        std::vector<UObject*>& candidates = scratchCandidates;
        candidates.clear();
        {
            DX_ZONE_NAMED("Actor visibility and occlusion");
            for (UObject* actor : actors) {
                if (!actor || actor->destroyed) {
                    continue;
                }
                objects::ensureLoaded(actor);
                if (!actor->data || !actor->isA(nameActor)) {
                    continue;
                }
                if (actor::bHidden(actor) || actor::bDeleteMe(actor)) {
                    continue;
                }
                if (actor == viewActor && !behindView) {
                    continue;
                }
                UObject* owner = actor::Owner(actor);
                if (actor::bOnlyOwnerSee(actor) && (owner != viewActor || behindView)) {
                    continue;
                }
                if (actor::bOwnerNoSee(actor) && owner == viewActor && viewActor && !behindView) {
                    continue;
                }
                bool iterated = iteratorHook && iteratorHook(actor, [&](UObject* item) {
                    uint8_t itemZone = actorZone(item);
                    if (itemZone != 0 && !(view.zoneMask & (1ull << (itemZone & 63)))) {
                        return;
                    }
                    float radius = actorRadius(item);
                    if (!inFrustum(view, actor::Location(item), radius) || !sphereReachesVisible(view, actor::Location(item), radius)) {
                        return;
                    }
                    if (levelData.levelInfo) {
                        actor::LastRenderTime(item) = levelinfo::TimeSeconds(levelData.levelInfo);
                    }
                    setActorOrder(item, view);
                    drawActor(item, view);
                });
                if (iterated) {
                    continue;
                }
                uint8_t zone = actorZone(actor);
                if (zone != 0 && !(view.zoneMask & (1ull << (zone & 63)))) {
                    continue;
                }
                float radius = actorRadius(actor);
                if (!inFrustum(view, actor::Location(actor), radius) || !sphereReachesVisible(view, actor::Location(actor), radius)) {
                    continue;
                }
                candidates.push_back(actor);
            }
            if (occlude) {
                // The GPU-query part, apart from the CPU culling above (zones, frustum, leaves).
                DX_ZONE_NAMED("Occlusion queries");
                cullOccluded(candidates, view);
            }
            renderCounters.visibleActors += static_cast<uint32_t>(candidates.size());
        }
        std::vector<UObject*>& decals = scratchDecals;
        decals.clear();
        {
            DX_ZONE_NAMED("Actor drawing");
            for (UObject* actor : candidates) {
                if (actor->isA(nameDecal)) {
                    decals.push_back(actor);
                    continue;
                }
                if (levelData.levelInfo && !(occlude && occlusionUnconfirmed.count(actor))) {
                    actor::LastRenderTime(actor) = levelinfo::TimeSeconds(levelData.levelInfo);
                }
                setActorOrder(actor, view);
                drawActor(actor, view);
            }
            device->setOcclusionSource(OcclusionSource::World);
            flushBatches(brushBatches);
            for (UObject* actor : decals) {
                drawDecal(actor, view);
            }
        }
        device->setOcclusionSource(OcclusionSource::Mesh);
        DX_ZONE_NAMED("Mesh batch submission");
        flushMeshBatches();
    }
}

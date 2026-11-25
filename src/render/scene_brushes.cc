#include "render/scene_renderer.h"

#include "core/objects.h"
#include "engine/props_engine.h"
#include "engine/world.h"
#include "render/lighting.h"
#include "render/scene_renderer_internal.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dx {
    void SceneRenderer::drawMoverSurfaces(UObject* actor, UModel* brush)
    {
        BrushDrawCache& cache = brushDrawCaches[actor];
        if (!cache.frameModel || cache.brush != brush) {
            cache.frameModel = std::make_shared<UModel>();
            UModel& frame = *cache.frameModel;
            frame.lightMaps = brush->lightMaps;
            frame.lightBits = brush->lightBits;
            frame.lights = brush->lights;
            for (const Poly& poly : brush->polys->polys) {
                BspSurf surf;
                surf.pBase = static_cast<int32_t>(frame.points.size());
                frame.points.push_back(poly.base);
                surf.vNormal = static_cast<int32_t>(frame.vectors.size());
                frame.vectors.push_back(poly.normal);
                surf.vTextureU = static_cast<int32_t>(frame.vectors.size());
                frame.vectors.push_back(poly.textureU);
                surf.vTextureV = static_cast<int32_t>(frame.vectors.size());
                frame.vectors.push_back(poly.textureV);
                surf.lightMap = poly.brushPoly >= 0 && static_cast<size_t>(poly.brushPoly) < brush->lightMaps.size() ? poly.brushPoly : -1;
                surf.texture = poly.texture;
                surf.polyFlags = poly.polyFlags;
                surf.panU = poly.panU;
                surf.panV = poly.panV;
                frame.surfs.push_back(surf);
            }
            cache.atlas.reset();
            cache.valid = false;
        }
        UModel* frame = cache.frameModel.get();
        LightmapTransform transform;
        transform.identity = false;
        transform.location = actor::Location(actor);
        transform.rotation = rotationAxes(actor::Rotation(actor));
        transform.scale = scaleOf(brush::MainScale(actor));
        transform.prePivot = actor::PrePivot(actor);
        Rotator actorRotation = actor::Rotation(actor);
        UTexture* matrix = matrixTexture(sceneWorld ? sceneWorld->player() : nullptr);
        // The groups keep texture ids, which a collected texture takes with it.
        uint64_t textureGeneration = device->textureGeneration();
        bool moved = cache.textureOverride != matrix || cache.textureGeneration != textureGeneration || !cache.valid || cache.brush != brush || !sameVector(cache.location, transform.location) || cache.rotation.pitch != actorRotation.pitch || cache.rotation.yaw != actorRotation.yaw || cache.rotation.roll != actorRotation.roll || !sameVector(cache.scale, transform.scale) || !sameVector(cache.prePivot, transform.prePivot);
        bool dynamicMover = actor->isA(nameMover) && mover::bDynamicLightMover(actor);
        const std::vector<Poly>& polys = brush->polys->polys;
        if (!cache.atlas) {
            cache.atlas = std::make_shared<LightmapAtlas>(*device, 256);
        }
        std::vector<Vector> surfAmbient;
        if (moved || !cache.atlas->built()) {
            surfAmbient.resize(polys.size());
            for (size_t i = 0; i < polys.size(); i++) {
                if (polys[i].vertices.empty()) {
                    continue;
                }
                Vector center;
                for (const Vector& point : polys[i].vertices) {
                    center += point;
                }
                center = transform.point(center / static_cast<float>(polys[i].vertices.size()));
                surfAmbient[i] = zoneAmbient(zoneActor(pointZone(center + transform.direction(polys[i].normal) * 2.0f)));
            }
        }
        if (!cache.atlas->built() || (dynamicMover && moved)) {
            if (dynamicMover) {
                std::vector<UObject*> moverLights;
                UModel* model = levelData.model;
                int32_t leaf = *reinterpret_cast<int32_t*>(actor::Region(actor) + sizeof(UObject*));
                if (leaf < 0) {
                    leaf = pointLeaf(transform.location);
                }
                if (model && leaf >= 0 && static_cast<size_t>(leaf) < model->leaves.size() && model->leaves[static_cast<size_t>(leaf)].permeating >= 0) {
                    for (size_t i = static_cast<size_t>(model->leaves[static_cast<size_t>(leaf)].permeating); i < model->lights.size() && model->lights[i]; i++) {
                        UObject* light = model->lights[i];
                        if (light->data && (actor::bSpecialLit(light) != 0) == (actor::bSpecialLit(actor) != 0)) {
                            moverLights.push_back(light);
                        }
                    }
                }
                cache.atlas->build(frame, surfAmbient, static_cast<float>(time), transform, &moverLights);
            } else {
                cache.atlas->build(frame, surfAmbient, static_cast<float>(time), transform, nullptr);
            }
            moved = true;
        }
        if (cache.litFrame != static_cast<uint64_t>(lightFrameCount)) {
            cache.litFrame = static_cast<uint64_t>(lightFrameCount);
            cache.atlas->animate(frame, static_cast<float>(time), static_cast<uint64_t>(lightFrameCount), [](size_t, uint64_t) {
                return true;
            });
        }
        if (moved) {
            cache.valid = true;
            cache.textureOverride = matrix;
            cache.textureGeneration = textureGeneration;
            cache.brush = brush;
            cache.location = transform.location;
            cache.rotation = actorRotation;
            cache.scale = transform.scale;
            cache.prePivot = transform.prePivot;
            cache.groups.clear();
            std::unordered_map<uint64_t, size_t> groupIndex;
            std::vector<GLVertex> polygon;
            for (size_t polyNumber = 0; polyNumber < polys.size(); polyNumber++) {
                const Poly& poly = polys[polyNumber];
                const BspSurf& surf = frame->surfs[polyNumber];
                if (poly.vertices.size() < 3) {
                    continue;
                }
                UTexture* texture = matrix ? matrix : asTexture(poly.texture);
                uint32_t flags = poly.polyFlags | textureFlags(texture);
                // A portal can reveal the next zone and still have a visible surface.
                // Skip its face only if it is marked invisible.
                if (flags & PF_Invisible) {
                    continue;
                }
                const GLTextureInfo* info = device->texture(device->animated(texture));
                float texWidth = texture && texture->width() ? static_cast<float>(texture->width()) : 64.0f;
                float texHeight = texture && texture->height() ? static_cast<float>(texture->height()) : 64.0f;
                float drawScale = texture && texture->data && texture::DrawScale(texture) > 0 ? texture::DrawScale(texture) : 1.0f;
                Vector normal = transform.direction(poly.normal);
                bool unlit = (flags & (PF_Unlit | PF_Modulated)) != 0;
                int32_t page = -1;
                polygon.clear();
                for (const Vector& point : poly.vertices) {
                    Vector world = transform.point(point);
                    GLVertex vertex;
                    vertex.x = world.x;
                    vertex.y = world.y;
                    vertex.z = world.z;
                    vertex.nx = normal.x;
                    vertex.ny = normal.y;
                    vertex.nz = normal.z;
                    vertex.u = ((point - poly.base).dot(poly.textureU) + static_cast<float>(poly.panU)) / (texWidth * drawScale);
                    vertex.v = ((point - poly.base).dot(poly.textureV) + static_cast<float>(poly.panV)) / (texHeight * drawScale);
                    if (!unlit) {
                        int32_t vertexPage = -1;
                        if (cache.atlas->texCoord(frame, surf, point, vertex.lu, vertex.lv, vertexPage)) {
                            page = vertexPage;
                        } else {
                            const Vector& surfaceAmbient = surfAmbient[polyNumber];
                            Vector level = Vector { std::floor(surfaceAmbient.x * 64.0f), std::floor(surfaceAmbient.y * 64.0f), std::floor(surfaceAmbient.z * 64.0f) } * (2.0f / 127.5f);
                            vertex.r = std::min(level.x, 1.0f);
                            vertex.g = std::min(level.y, 1.0f);
                            vertex.b = std::min(level.z, 1.0f);
                        }
                    }
                    polygon.push_back(vertex);
                }
                uint32_t textureId = info ? info->id() : device->whiteTexture();
                uint32_t lightmapId = page >= 0 ? cache.atlas->texture(page) : 0;
                bool translucent = isTranslucentFlags(flags);
                uint32_t mode = translucent ? ((flags & PF_Translucent) ? 1u : 2u) : ((flags & PF_Masked) ? 3u : 0u);
                bool mirror = (flags & PF_Mirrored) != 0;
                uint64_t key = (1ull << 63) | (static_cast<uint64_t>(textureId) << 32) | (static_cast<uint64_t>(lightmapId) << 8) | mode | ((flags & PF_TwoSided) ? 4u : 0u) | ((flags & PF_SpecialLit) ? 8u : 0u) | (mirror ? 16u : 0u);
                auto found = groupIndex.find(key);
                if (found == groupIndex.end()) {
                    EmittedGroup group;
                    group.key = key;
                    group.flags = flags & (PF_Translucent | PF_Modulated | PF_Masked | PF_SpecialLit);
                    group.texture = textureId;
                    group.translucent = translucent;
                    group.lightmap = lightmapId;
                    group.cull = !(flags & PF_TwoSided);
                    group.brush = true;
                    group.mirror = mirror;
                    cache.groups.push_back(std::move(group));
                    found = groupIndex.emplace(key, cache.groups.size() - 1).first;
                }
                std::vector<GLVertex>& out = cache.groups[found->second].vertices;
                for (size_t i = 1; i + 1 < polygon.size(); i++) {
                    out.push_back(polygon[0]);
                    out.push_back(polygon[i]);
                    out.push_back(polygon[i + 1]);
                }
            }
            for (EmittedGroup& group : cache.groups) {
                group.low = { group.vertices[0].x, group.vertices[0].y, group.vertices[0].z };
                group.high = group.low;
                for (const GLVertex& vertex : group.vertices) {
                    group.low = { std::min(group.low.x, vertex.x), std::min(group.low.y, vertex.y), std::min(group.low.z, vertex.z) };
                    group.high = { std::max(group.high.x, vertex.x), std::max(group.high.y, vertex.y), std::max(group.high.z, vertex.z) };
                }
            }
        }
        for (EmittedGroup& group : cache.groups) {
            group.lights.clear();
            if (group.lightmap) {
                litLights(group.low, group.high, (group.flags & PF_SpecialLit) != 0, group.lights);
            }
        }
        appendGroups(cache.groups, brushBatches);
    }

    void SceneRenderer::drawBrush(UObject* actor, const View&)
    {
        UObject* brushObject = actor::Brush(actor);
        if (!brushObject) {
            return;
        }
        objects::ensureLoaded(brushObject);
        UModel* brush = cast<UModel>(brushObject);
        if (!brush) {
            return;
        }
        if (!brush->polys) {
            return;
        }
        objects::ensureLoaded(brush->polys);
        if (!brush->lightMaps.empty()) {
            drawMoverSurfaces(actor, brush);
            return;
        }
        objects::ensureLoaded(brush->polys);
        Rotator actorRotation = actor::Rotation(actor);
        Vector scale = scaleOf(brush::MainScale(actor));
        Vector location = actor::Location(actor);
        Vector prePivot = actor::PrePivot(actor);
        BrushDrawCache& cache = brushDrawCaches[actor];
        bool sameGeometry = cache.valid && cache.brush == brush && sameVector(cache.location, location) && cache.rotation.pitch == actorRotation.pitch && cache.rotation.yaw == actorRotation.yaw && cache.rotation.roll == actorRotation.roll && sameVector(cache.scale, scale) && sameVector(cache.prePivot, prePivot);
        Coords rotation = rotationAxes(actorRotation);
        std::vector<Vector>& world = scratchWorld;
        auto computePoints = [&]() {
            world.clear();
            for (const Poly& poly : brush->polys->polys) {
                for (const Vector& point : poly.vertices) {
                    world.push_back(location + untransformVector(rotation, (point - prePivot) * scale));
                }
            }
        };
        Vector sphereCenter = cache.sphereCenter;
        float sphereRadius = cache.sphereRadius;
        if (!sameGeometry) {
            computePoints();
            boundingSphere(world, sphereCenter, sphereRadius);
        }
        selectLights(sphereCenter, sphereRadius);
        UObject* zone = zoneForActor(actor);
        Vector ambient = zoneAmbient(zone);

        UTexture* matrix = matrixTexture(sceneWorld ? sceneWorld->player() : nullptr);
        std::vector<const GLTextureInfo*>& polyInfo = scratchSlotInfo;
        std::vector<uint32_t>& polyFlags = scratchSlotFlags;
        polyInfo.assign(brush->polys->polys.size(), nullptr);
        polyFlags.assign(brush->polys->polys.size(), 0);
        for (size_t i = 0; i < brush->polys->polys.size(); i++) {
            const Poly& poly = brush->polys->polys[i];
            if (poly.vertices.size() < 3) {
                continue;
            }
            UTexture* texture = device->animated(matrix ? matrix : asTexture(poly.texture));
            polyFlags[i] = poly.polyFlags | textureFlags(texture);
            // Portal faces are drawn, as in drawMoverSurfaces.
            if (polyFlags[i] & PF_Invisible) {
                continue;
            }
            polyInfo[i] = device->texture(texture);
        }

        bool cacheable = true;
        for (const PreparedLight& prepared : actorLightPrepared) {
            if (timeDependentEffect(prepared.effect)) {
                cacheable = false;
            }
        }
        uint64_t textureGeneration = device->textureGeneration();
        if (cacheable && sameGeometry && cache.textureGeneration == textureGeneration && cache.polyInfo == polyInfo && cache.polyFlags == polyFlags && sameVector(cache.ambient, ambient) && sameLights(cache.lights)) {
            appendGroups(cache.groups, brushBatches);
            return;
        }
        if (sameGeometry) {
            computePoints();
        }
        cache.valid = cacheable;
        cache.location = location;
        cache.rotation = actorRotation;
        cache.scale = scale;
        cache.prePivot = prePivot;
        cache.brush = brush;
        cache.sphereCenter = sphereCenter;
        cache.sphereRadius = sphereRadius;
        cache.polyInfo = polyInfo;
        cache.polyFlags = polyFlags;
        cache.textureGeneration = textureGeneration;
        cache.ambient = ambient;
        storeLights(cache.lights);
        cache.groups.clear();
        std::unordered_map<uint64_t, size_t> groupIndex;

        size_t pointIndex = 0;
        std::vector<GLVertex> polygon;
        for (size_t polyNumber = 0; polyNumber < brush->polys->polys.size(); polyNumber++) {
            const Poly& poly = brush->polys->polys[polyNumber];
            size_t firstPoint = pointIndex;
            pointIndex += poly.vertices.size();
            if (poly.vertices.size() < 3) {
                continue;
            }
            uint32_t flags = polyFlags[polyNumber];
            if (flags & PF_Invisible) {
                continue;
            }
            const GLTextureInfo* info = polyInfo[polyNumber];
            float width = info ? static_cast<float>(info->width) : 64.0f;
            float height = info ? static_cast<float>(info->height) : 64.0f;
            UTexture* texture = device->animated(matrix ? matrix : asTexture(poly.texture));
            float drawScale = texture && texture->data ? std::max(texture::DrawScale(texture), 0.01f) : 1.0f;
            Vector normal = untransformVector(rotation, poly.normal * Vector { 1.0f / scale.x, 1.0f / scale.y, 1.0f / scale.z }).normal();
            bool translucent = isTranslucentFlags(flags);
            bool unlit = (flags & PF_Unlit) || translucent;
            polygon.clear();
            for (size_t i = 0; i < poly.vertices.size(); i++) {
                const Vector& point = poly.vertices[i];
                const Vector& worldPoint = world[firstPoint + i];
                GLVertex vertex;
                vertex.x = worldPoint.x;
                vertex.y = worldPoint.y;
                vertex.z = worldPoint.z;
                vertex.u = ((point - poly.base).dot(poly.textureU) + static_cast<float>(poly.panU)) / (width * drawScale);
                vertex.v = ((point - poly.base).dot(poly.textureV) + static_cast<float>(poly.panV)) / (height * drawScale);
                Vector color = actorLight(actor, worldPoint, normal, ambient, 1.0f, unlit, nullptr);
                vertex.r = color.x;
                vertex.g = color.y;
                vertex.b = color.z;
                polygon.push_back(vertex);
            }
            uint32_t textureId = info ? info->id() : device->whiteTexture();
            uint32_t mode = translucent ? ((flags & PF_Translucent) ? 1u : 2u) : ((flags & PF_Masked) ? 3u : 0u);
            bool mirror = (flags & PF_Mirrored) != 0;
            uint64_t key = (static_cast<uint64_t>(textureId) << 8) | mode | (mirror ? 4u : 0u);
            auto found = groupIndex.find(key);
            if (found == groupIndex.end()) {
                EmittedGroup group;
                group.key = key;
                group.flags = flags & (PF_Translucent | PF_Modulated | PF_Masked);
                group.texture = textureId;
                group.translucent = translucent;
                group.brush = true;
                group.mirror = mirror;
                cache.groups.push_back(std::move(group));
                found = groupIndex.emplace(key, cache.groups.size() - 1).first;
            }
            std::vector<GLVertex>& out = cache.groups[found->second].vertices;
            for (size_t i = 1; i + 1 < polygon.size(); i++) {
                out.push_back(polygon[0]);
                out.push_back(polygon[i]);
                out.push_back(polygon[i + 1]);
            }
        }
        appendGroups(cache.groups, brushBatches);
    }
}

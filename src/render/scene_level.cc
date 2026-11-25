#include "render/scene_renderer.h"

#include "core/objects.h"
#include "core/profile.h"
#include "engine/props_engine.h"
#include "engine/world.h"
#include "render/lighting.h"
#include "render/procedural.h"
#include "render/scene_renderer_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace dx {
    namespace {
        // The convex hull of points on a plane with this normal, in order around it.
        std::vector<Vector> hullOnPlane(const std::vector<Vector>& points, const Vector& normal)
        {
            Vector tangent = std::abs(normal.z) < 0.9f ? Vector { 0, 0, 1 }.cross(normal).normal() : Vector { 1, 0, 0 }.cross(normal).normal();
            Vector bitangent = normal.cross(tangent);
            std::vector<std::pair<std::pair<float, float>, Vector>> projected;
            for (const Vector& point : points) {
                projected.push_back({ { point.dot(tangent), point.dot(bitangent) }, point });
            }
            std::sort(projected.begin(), projected.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            auto turn = [](const std::pair<float, float>& o, const std::pair<float, float>& a, const std::pair<float, float>& b) {
                return (a.first - o.first) * (b.second - o.second) - (a.second - o.second) * (b.first - o.first);
            };
            std::vector<size_t> chain;
            for (int pass = 0; pass < 2; pass++) {
                size_t start = chain.size();
                for (size_t k = 0; k < projected.size(); k++) {
                    size_t i = pass == 0 ? k : projected.size() - 1 - k;
                    while (chain.size() >= start + 2 && turn(projected[chain[chain.size() - 2]].first, projected[chain.back()].first, projected[i].first) <= 0.01f) {
                        chain.pop_back();
                    }
                    chain.push_back(i);
                }
                chain.pop_back();
            }
            std::vector<Vector> hull;
            for (size_t i : chain) {
                hull.push_back(projected[i].second);
            }
            return hull;
        }
    }

    void SceneRenderer::buildLevel(ULevel* level, const std::vector<UObject*>& actors)
    {
        flushLevel();
        levelData.level = level;
        levelData.model = level->model;
        objects::ensureLoaded(level->model);
        for (UObject* actor : actors) {
            if (actor && actor->isA(nameLevelInfo)) {
                levelData.levelInfo = actor;
                break;
            }
        }
        UModel* model = level->model;
        if (!model) {
            return;
        }

        std::vector<int32_t> surfZone(model->surfs.size(), -1);
        for (const BspNode& node : model->nodes) {
            if (node.surf >= 0 && static_cast<size_t>(node.surf) < surfZone.size() && surfZone[static_cast<size_t>(node.surf)] < 0) {
                surfZone[static_cast<size_t>(node.surf)] = node.zone[1];
            }
        }
        std::vector<Vector> surfAmbient(model->surfs.size());
        for (size_t i = 0; i < model->surfs.size(); i++) {
            surfAmbient[i] = zoneAmbient(zoneActor(static_cast<uint8_t>(std::max(surfZone[i], 0))));
        }
        std::vector<LightmapVariant> variants;
        std::set<std::pair<int32_t, uint8_t>> variantKeys;
        for (const BspNode& node : model->nodes) {
            if (node.surf < 0 || static_cast<size_t>(node.surf) >= model->surfs.size()) {
                continue;
            }
            const BspSurf& surf = model->surfs[static_cast<size_t>(node.surf)];
            for (int side = 1; side >= 0; side--) {
                if (side == 0 && !(surf.polyFlags & (PF_TwoSided | PF_Portal))) {
                    continue;
                }
                uint8_t zone = node.zone[side];
                Vector ambient = zoneAmbient(zoneActor(zone));
                if (sameVector(ambient, surfAmbient[static_cast<size_t>(node.surf)])) {
                    continue;
                }
                if (variantKeys.insert({ node.surf, zone }).second) {
                    variants.push_back({ node.surf, zone, ambient });
                }
            }
        }
        lightmaps.build(model, surfAmbient, static_cast<float>(time), {}, nullptr, &variants);
        levelData.fogFrame.assign(model->surfs.size(), 0);
        levelData.fogCache.assign(model->surfs.size(), {});
        levelData.fogged.assign(model->surfs.size(), 0);
        levelData.surfNodes.assign(model->surfs.size(), {});
        for (size_t i = 0; i < model->nodes.size(); i++) {
            int32_t surf = model->nodes[i].surf;
            if (surf >= 0 && static_cast<size_t>(surf) < model->surfs.size()) {
                levelData.surfNodes[static_cast<size_t>(surf)].push_back(static_cast<int32_t>(i));
            }
        }
        levelData.surfFlags.assign(model->surfs.size(), 0);
        for (size_t i = 0; i < model->surfs.size(); i++) {
            UTexture* texture = asTexture(model->surfs[i].texture);
            if (!texture) {
                texture = levelData.levelInfo ? asTexture(levelinfo::DefaultTexture(levelData.levelInfo)) : nullptr;
            }
            levelData.surfFlags[i] = model->surfs[i].polyFlags | textureFlags(texture);
        }

        struct Key {
            UTexture* texture;
            uint32_t flags;
            int32_t page;
            uint8_t zone;

            bool operator<(const Key& o) const
            {
                if (texture != o.texture) {
                    return texture < o.texture;
                }
                if (flags != o.flags) {
                    return flags < o.flags;
                }
                if (page != o.page) {
                    return page < o.page;
                }
                return zone < o.zone;
            }
        };

        std::map<Key, std::vector<GLVertex>> groups;
        std::map<UTexture*, std::pair<float, float>> texelSizes;
        std::map<Key, std::vector<GLVertex>> detailGroups;
        std::map<std::pair<int32_t, uint8_t>, std::vector<GLVertex>> mirrorGroups;
        std::vector<std::pair<int32_t, std::pair<int32_t, uint8_t>>> mirrorNodes;
        std::map<Key, std::vector<std::array<int32_t, 3>>> detailSizes;
        std::map<Key, std::vector<std::array<int32_t, 6>>> surfRanges;
        std::map<Key, std::vector<std::array<int32_t, 3>>> nodeRanges;
        for (const BspNode& node : model->nodes) {
            if (node.numVertices < 3 || node.surf < 0 || static_cast<size_t>(node.surf) >= model->surfs.size()) {
                continue;
            }
            const BspSurf& surf = model->surfs[static_cast<size_t>(node.surf)];
            if (surf.polyFlags & PF_Invisible) {
                continue;
            }
            if (surf.polyFlags & PF_FakeBackdrop) {
                levelData.hasBackdrop = true;
            }
            UTexture* texture = asTexture(surf.texture);
            if (!texture) {
                texture = levelData.levelInfo ? asTexture(levelinfo::DefaultTexture(levelData.levelInfo)) : nullptr;
            }
            float texWidth = texture && texture->width() ? static_cast<float>(texture->width()) : 64.0f;
            float texHeight = texture && texture->height() ? static_cast<float>(texture->height()) : 64.0f;
            if (texture && ProceduralTextures::isProcedural(texture)) {
                texWidth = static_cast<float>(std::max(bitmap::UClamp(texture), 1));
                texHeight = static_cast<float>(std::max(bitmap::VClamp(texture), 1));
                if (!texture->mips.empty() && texture->mips[0].width > 0) {
                    texWidth = static_cast<float>(texture->mips[0].width);
                    texHeight = static_cast<float>(texture->mips[0].height);
                }
            }
            float drawScale = texture && texture->data ? texture::DrawScale(texture) : 1.0f;
            if (drawScale <= 0) {
                drawScale = 1;
            }
            texelSizes[texture] = { texWidth * drawScale, texHeight * drawScale };
            const Vector& base = model->points.at(static_cast<size_t>(surf.pBase));
            const Vector& axisU = model->vectors.at(static_cast<size_t>(surf.vTextureU));
            const Vector& axisV = model->vectors.at(static_cast<size_t>(surf.vTextureV));
            Vector surfaceNormal = model->vectors.at(static_cast<size_t>(surf.vNormal)).normal();
            uint32_t combinedFlags = surf.polyFlags | textureFlags(texture);
            bool unlit = (combinedFlags & (PF_Unlit | PF_Modulated)) != 0;
            auto emit = [&](uint8_t zone, const Vector& sideAmbient, uint32_t flagMask, bool reversed) {
                int32_t page = -1;
                std::vector<GLVertex> polygon;
                for (uint8_t i = 0; i < node.numVertices; i++) {
                    const BspVert& vert = model->verts.at(static_cast<size_t>(node.vertPool) + i);
                    const Vector& point = model->points.at(static_cast<size_t>(vert.vertex));
                    GLVertex vertex;
                    vertex.x = point.x;
                    vertex.y = point.y;
                    vertex.z = point.z;
                    vertex.nx = surfaceNormal.x;
                    vertex.ny = surfaceNormal.y;
                    vertex.nz = surfaceNormal.z;
                    vertex.u = ((point - base).dot(axisU) + static_cast<float>(surf.panU)) / (texWidth * drawScale);
                    vertex.v = ((point - base).dot(axisV) + static_cast<float>(surf.panV)) / (texHeight * drawScale);
                    if (!unlit) {
                        int32_t vertexPage = -1;
                        if (lightmaps.texCoord(model, surf, point, vertex.lu, vertex.lv, vertexPage, zone)) {
                            page = vertexPage;
                        } else {
                            const Vector& zoneLight = sideAmbient;
                            Vector ambient = Vector { std::floor(zoneLight.x * 64.0f), std::floor(zoneLight.y * 64.0f), std::floor(zoneLight.z * 64.0f) } * (2.0f / 127.5f);
                            vertex.r = std::min(ambient.x, 1.0f);
                            vertex.g = std::min(ambient.y, 1.0f);
                            vertex.b = std::min(ambient.z, 1.0f);
                        }
                    }
                    polygon.push_back(vertex);
                }
                if (reversed) {
                    std::reverse(polygon.begin(), polygon.end());
                }
                Key key { texture, (surf.polyFlags | textureFlags(texture)) & flagMask, page, zone };
                if ((surf.polyFlags & PF_Mirrored) && !reversed) {
                    std::vector<GLVertex>& mirrorGroup = mirrorGroups[{ node.surf, zone }];
                    mirrorNodes.push_back({ static_cast<int32_t>(&node - model->nodes.data()), { node.surf, zone } });
                    for (size_t i = 1; i + 1 < polygon.size(); i++) {
                        mirrorGroup.push_back(polygon[0]);
                        mirrorGroup.push_back(polygon[i]);
                        mirrorGroup.push_back(polygon[i + 1]);
                    }
                }
                std::vector<GLVertex>& group = groups[key];
                if (polygon.size() >= 3) {
                    surfRanges[key].push_back({ static_cast<int32_t>(group.size()), static_cast<int32_t>((polygon.size() - 2) * 3), node.surf, node.zone[0], node.zone[1], static_cast<int32_t>(&node - model->nodes.data()) });
                    nodeRanges[key].push_back({ static_cast<int32_t>(group.size()), static_cast<int32_t>((polygon.size() - 2) * 3), static_cast<int32_t>(&node - model->nodes.data()) });
                }
                for (size_t i = 1; i + 1 < polygon.size(); i++) {
                    group.push_back(polygon[0]);
                    group.push_back(polygon[i]);
                    group.push_back(polygon[i + 1]);
                }
                UTexture* detail = texture && texture->data && !(surf.polyFlags & PF_Portal) ? asTexture(texture::DetailTexture(texture)) : nullptr;
                if (detail && polygon.size() >= 3) {
                    float detailScale = detail->data && texture::DrawScale(detail) > 0 ? texture::DrawScale(detail) : 1.0f;
                    float detailWidth = static_cast<float>(std::max(detail->width(), 1u)) * detailScale;
                    float detailHeight = static_cast<float>(std::max(detail->height(), 1u)) * detailScale;
                    std::vector<GLVertex>& detailGroup = detailGroups[key];
                    for (size_t i = 1; i + 1 < polygon.size(); i++) {
                        for (size_t corner : { size_t { 0 }, i, i + 1 }) {
                            GLVertex vertex = polygon[corner];
                            Vector point { vertex.x, vertex.y, vertex.z };
                            vertex.u = (point - base).dot(axisU) / detailWidth;
                            vertex.v = (point - base).dot(axisV) / detailHeight;
                            detailGroup.push_back(vertex);
                        }
                    }
                    detailSizes[key].push_back({ static_cast<int32_t>((polygon.size() - 2) * 3), node.surf, static_cast<int32_t>(&node - model->nodes.data()) });
                }
            };
            uint8_t frontZone = node.zone[1];
            uint8_t backZone = node.zone[0];
            Vector frontAmbient = zoneAmbient(zoneActor(frontZone));
            Vector backAmbient = zoneAmbient(zoneActor(backZone));
            if ((surf.polyFlags & (PF_TwoSided | PF_Portal)) && !unlit && !sameVector(frontAmbient, backAmbient)) {
                emit(frontZone, frontAmbient, ~PF_TwoSided, false);
                emit(backZone, backAmbient, ~PF_TwoSided, true);
            } else {
                emit(frontZone, frontAmbient, ~0u, false);
            }
        }

        std::vector<GLVertex> all;
        std::vector<GLVertex> detailVertices;
        for (auto& [key, vertices] : groups) {
            Batch batch;
            batch.texture = key.texture;
            batch.polyFlags = key.flags;
            batch.lightmapPage = key.page;
            batch.zone = key.zone;
            auto isFogZone = [&](int32_t zone) {
                UObject* zoneObject = zoneActor(static_cast<uint8_t>(zone));
                return zoneObject && zoneObject->data && zoneObject->isA(nameZoneInfo) && zoneinfo::bFogZone(zoneObject);
            };
            for (std::array<int32_t, 6> range : surfRanges[key]) {
                if (isFogZone(range[3]) || isFogZone(range[4])) {
                    batch.fogZone = true;
                }
            }
            if (batch.fogZone) {
                for (std::array<int32_t, 6> range : surfRanges[key]) {
                    range[0] += static_cast<int32_t>(all.size());
                    range[3] = isFogZone(range[3]) ? 1 : 0;
                    range[4] = isFogZone(range[4]) ? 1 : 0;
                    batch.surfPolys.push_back(range);
                }
            }
            for (std::array<int32_t, 3> range : nodeRanges[key]) {
                range[0] += static_cast<int32_t>(all.size());
                batch.nodePolys.push_back(range);
            }
            batch.uSize = texelSizes[key.texture].first;
            batch.vSize = texelSizes[key.texture].second;
            auto detailGroup = detailGroups.find(key);
            if (detailGroup != detailGroups.end()) {
                batch.detail = asTexture(texture::DetailTexture(key.texture));
                int32_t at = static_cast<int32_t>(detailVertices.size());
                for (const auto& [size, surfIndex, node] : detailSizes[key]) {
                    batch.detailPolys.push_back({ at, size, surfIndex, node });
                    at += size;
                }
                for (const GLVertex& vertex : detailGroup->second) {
                    levelData.detailPoints.push_back({ vertex.x, vertex.y, vertex.z });
                }
                detailVertices.insert(detailVertices.end(), detailGroup->second.begin(), detailGroup->second.end());
            }
            batch.first = static_cast<int32_t>(all.size());
            batch.count = static_cast<int32_t>(vertices.size());
            batch.boundsMin = { vertices[0].x, vertices[0].y, vertices[0].z };
            batch.boundsMax = batch.boundsMin;
            for (const GLVertex& vertex : vertices) {
                batch.boundsMin = { std::min(batch.boundsMin.x, vertex.x), std::min(batch.boundsMin.y, vertex.y), std::min(batch.boundsMin.z, vertex.z) };
                batch.boundsMax = { std::max(batch.boundsMax.x, vertex.x), std::max(batch.boundsMax.y, vertex.y), std::max(batch.boundsMax.z, vertex.z) };
            }
            all.insert(all.end(), vertices.begin(), vertices.end());
            levelData.batches.push_back(batch);
        }
        if (!all.empty()) {
            levelData.geometry = device->createStatic(all);
        }
        levelData.vertices = all;
        for (const BspNode& node : model->nodes) {
            if (node.numVertices < 3 || node.surf < 0 || static_cast<size_t>(node.surf) >= model->surfs.size()) {
                continue;
            }
            std::vector<GLVertex> polygon;
            for (uint8_t i = 0; i < node.numVertices; i++) {
                const Vector& point = model->points.at(static_cast<size_t>(model->verts.at(static_cast<size_t>(node.vertPool) + i).vertex));
                GLVertex vertex;
                vertex.x = point.x;
                vertex.y = point.y;
                vertex.z = point.z;
                polygon.push_back(vertex);
            }
            for (size_t i = 0; i < polygon.size(); i++) {
                levelData.wireVertices.push_back(polygon[i]);
                levelData.wireVertices.push_back(polygon[(i + 1) % polygon.size()]);
            }
            if (model->surfs[static_cast<size_t>(node.surf)].polyFlags & PF_Portal) {
                for (size_t i = 1; i + 1 < polygon.size(); i++) {
                    levelData.portalVertices.push_back(polygon[0]);
                    levelData.portalVertices.push_back(polygon[i]);
                    levelData.portalVertices.push_back(polygon[i + 1]);
                }
            }
        }
        if (!detailVertices.empty()) {
            levelData.detailGeometry = device->createStatic(detailVertices);
        }
        levelData.nodeEmitStart.assign(model->nodes.size() + 1, 0);
        for (const Batch& batch : levelData.batches) {
            for (const auto& [first, count, node] : batch.nodePolys) {
                levelData.nodeEmitStart[static_cast<size_t>(node) + 1]++;
            }
        }
        for (size_t i = 0; i < model->nodes.size(); i++) {
            levelData.nodeEmitStart[i + 1] += levelData.nodeEmitStart[i];
        }
        levelData.nodeEmits.resize(static_cast<size_t>(levelData.nodeEmitStart.back()));
        std::vector<int32_t> emitFill(levelData.nodeEmitStart.begin(), levelData.nodeEmitStart.end() - 1);
        for (size_t b = 0; b < levelData.batches.size(); b++) {
            for (const auto& [first, count, node] : levelData.batches[b].nodePolys) {
                levelData.nodeEmits[static_cast<size_t>(emitFill[static_cast<size_t>(node)]++)] = { static_cast<int32_t>(b), first, count };
            }
        }
        levelData.nodeStamp.assign(model->nodes.size(), 0);
        levelData.surfStamp.assign(model->surfs.size(), 0);
        levelData.leafStamp.assign(model->leaves.size(), 0);
        levelData.nodeVolumetrics.assign(model->nodes.size(), 0);
        levelData.leafVolumetrics.assign(model->leaves.size(), 0);
        for (const ConvexLeaf& leaf : model->leaves) {
            levelData.anyVolumetric = levelData.anyVolumetric || leaf.volumetric >= 0;
        }
        levelData.cullStamp.assign(model->nodes.size(), 0);
        levelData.cullOrder.assign(model->nodes.size(), 0);
        nodeOrder.assign(model->nodes.size(), 0);
        leafOrder.assign(model->leaves.size(), 0);
        levelData.batchRanges.assign(levelData.batches.size(), {});
        levelData.nodeMirror.assign(model->nodes.size(), -1);
        std::map<std::pair<int32_t, uint8_t>, int32_t> mirrorIndex;
        std::vector<GLVertex> mirrorVertices;
        for (const auto& [key, vertices] : mirrorGroups) {
            mirrorIndex[key] = static_cast<int32_t>(levelData.mirrors.size());
            const BspSurf& surf = model->surfs[static_cast<size_t>(key.first)];
            MirrorSurface mirror;
            mirror.normal = model->vectors.at(static_cast<size_t>(surf.vNormal)).normal();
            mirror.distance = mirror.normal.dot(model->points.at(static_cast<size_t>(surf.pBase)));
            mirror.zone = key.second;
            mirror.polyFlags = surf.polyFlags | textureFlags(asTexture(surf.texture));
            mirror.first = static_cast<int32_t>(mirrorVertices.size());
            mirror.count = static_cast<int32_t>(vertices.size());
            std::vector<Vector> points;
            for (const GLVertex& vertex : vertices) {
                points.push_back({ vertex.x, vertex.y, vertex.z });
            }
            mirror.hull = hullOnPlane(points, mirror.normal);
            mirrorVertices.insert(mirrorVertices.end(), vertices.begin(), vertices.end());
            levelData.mirrors.push_back(mirror);
        }
        // Mirrors on one plane, in one zone, with the same two-sidedness, as groups that share one
        // mirror view; each group's hull is the hull of its pieces' hulls.
        levelData.mirrorPlanes.clear();
        for (size_t index = 0; index < levelData.mirrors.size(); index++) {
            const MirrorSurface& mirror = levelData.mirrors[index];
            MirrorPlane* found = nullptr;
            for (MirrorPlane& plane : levelData.mirrorPlanes) {
                const MirrorSurface& first = levelData.mirrors[static_cast<size_t>(plane.pieces.front())];
                if (first.zone == mirror.zone && ((first.polyFlags ^ mirror.polyFlags) & PF_TwoSided) == 0 && first.normal.dot(mirror.normal) > 0.9999f && std::abs(first.distance - mirror.distance) < 0.5f) {
                    found = &plane;
                    break;
                }
            }
            if (!found) {
                levelData.mirrorPlanes.emplace_back();
                found = &levelData.mirrorPlanes.back();
            }
            found->pieces.push_back(static_cast<int32_t>(index));
        }
        for (MirrorPlane& plane : levelData.mirrorPlanes) {
            std::vector<Vector> points;
            for (int32_t piece : plane.pieces) {
                const MirrorSurface& mirror = levelData.mirrors[static_cast<size_t>(piece)];
                points.insert(points.end(), mirror.hull.begin(), mirror.hull.end());
            }
            plane.hull = hullOnPlane(points, levelData.mirrors[static_cast<size_t>(plane.pieces.front())].normal);
        }
        for (const auto& [node, key] : mirrorNodes) {
            levelData.nodeMirror[static_cast<size_t>(node)] = mirrorIndex[key];
        }
        if (!mirrorVertices.empty()) {
            levelData.mirrorGeometry = device->createStatic(mirrorVertices);
        }
    }

    void SceneRenderer::applyBlend(uint32_t polyFlags, int32_t extraFlags)
    {
        int32_t flags = SHADER_Textured | extraFlags;
        if (renderMode == RenderMode::Unlit) {
            flags = (flags | SHADER_Fullbright) & ~(SHADER_Lightmap | SHADER_Dynamic);
        }
        if (polyFlags & PF_Translucent) {
            device->setBlend(BlendMode::Translucent);
        } else if (polyFlags & PF_Modulated) {
            device->setBlend(BlendMode::Modulated);
        } else if (polyFlags & PF_Masked) {
            device->setBlend(BlendMode::Masked);
            flags |= SHADER_AlphaTest;
        } else {
            device->setBlend(BlendMode::Opaque);
        }
        device->setFlags(flags);
        if (polyFlags & PF_TwoSided) {
            device->setCulling(false);
        } else {
            device->setCulling(true);
        }
    }

    void SceneRenderer::drawLevelGeometry(const View& view, bool translucentPass)
    {
        if (!levelData.geometry) {
            if (translucentPass) {
                flushTranslucent(view);
            }
            return;
        }
        device->useShader();
        device->setViewProj(view.viewProj);
        device->setTint(1, 1, 1, 1);
        UploadedLights uploadedLights;
        device->setDepthTest(true);
        device->setDepthFunc(DepthFunc::LessEqual);
        device->setFrontFace(!view.mirrored);
        device->bindStatic(levelData.geometry.id());
        if (!translucentPass) {
            {
                DX_ZONE_NAMED("Gather visible surfaces");
                gatherVisible(view);
            }
            if (renderDebug.lightmapAnimation) {
                DX_ZONE_NAMED("Lightmap animation");
                lightmaps.animate(levelData.model, static_cast<float>(time), static_cast<uint64_t>(lightFrameCount), [&](size_t surf, uint64_t zones) {
                    return (view.zoneMask & zones) && levelData.surfStamp[surf] == visibleStamp;
                });
            }
        }
        bool skyActive = view.sky;
        bool mirrorsActive = renderDebug.mirrors && view.recursion < MaxFrameRecursion - 1;
        if (translucentPass) {
            std::vector<TranslucentSurface>& surfaces = scratchTranslucentSurfaces;
            surfaces.clear();
            for (size_t index = 0; index < levelData.batches.size(); index++) {
                const Batch& batch = levelData.batches[index];
                std::vector<std::array<int32_t, 3>>& ranges = levelData.batchRanges[index];
                if (ranges.empty() || !isTranslucentFlags(batch.polyFlags) || !(view.zoneMask & (1ull << (batch.zone & 63)))) {
                    continue;
                }
                if ((batch.polyFlags & PF_FakeBackdrop) && skyActive) {
                    continue;
                }
                std::sort(ranges.begin(), ranges.end());
                for (const auto& [first, count, node] : ranges) {
                    surfaces.push_back({ nodeOrder[static_cast<size_t>(node)], &batch, first, count, node });
                }
            }
            std::stable_sort(surfaces.begin(), surfaces.end(), [](const TranslucentSurface& a, const TranslucentSurface& b) {
                return a.order < b.order;
            });
            // Translucent actors go into the same back-to-front order as the surfaces, as the
            // original's span renderer drew them: a carried decoration (made translucent) in front
            // of a translucent mirror floor would otherwise be blended over by the floor.
            sortTranslucent();
            const std::vector<size_t>& items = scratchTranslucentOrder;
            size_t nextItem = 0;
            auto drawItemsBefore = [&](int64_t order) {
                if (nextItem >= items.size() || translucentItems[items[nextItem]].order >= order) {
                    return;
                }
                device->setPan(0, 0);
                device->setScale(1, 1);
                device->setOcclusionSource(OcclusionSource::Mesh);
                while (nextItem < items.size() && translucentItems[items[nextItem]].order < order) {
                    drawTranslucentItem(items[nextItem++]);
                }
                // drawGroup may change the dynamic lights.
                uploadedLights.valid = false;
            };
            for (const TranslucentSurface& surface : surfaces) {
                drawItemsBefore(surface.order);
                drawBatch(*surface.batch, view, &surface.first, &surface.count, 1, surface.node, uploadedLights);
            }
            drawItemsBefore(std::numeric_limits<int64_t>::max());
            translucentCount = 0;
            device->setPan(0, 0);
            device->setScale(1, 1);
            device->setBlend(BlendMode::Opaque);
            return;
        }
        auto collectRanges = [&](int32_t index) {
            std::vector<std::array<int32_t, 3>>& ranges = levelData.batchRanges[static_cast<size_t>(index)];
            std::sort(ranges.begin(), ranges.end());
            scratchFirsts.clear();
            scratchCounts.clear();
            for (const auto& [first, count, node] : ranges) {
                if (!scratchFirsts.empty() && scratchFirsts.back() + scratchCounts.back() == first) {
                    scratchCounts.back() += count;
                } else {
                    scratchFirsts.push_back(first);
                    scratchCounts.push_back(count);
                }
            }
        };
        if (mirrorsActive && !levelData.mirrors.empty()) {
            device->setBlend(BlendMode::Opaque);
            device->setFlags(0);
            device->setColorWrite(false);
            for (int32_t index : touchedBatches) {
                const Batch& batch = levelData.batches[static_cast<size_t>(index)];
                if (!(batch.polyFlags & PF_Mirrored) || !(view.zoneMask & (1ull << (batch.zone & 63)))) {
                    continue;
                }
                if (batch.polyFlags & PF_TwoSided) {
                    device->setCulling(false);
                } else {
                    device->setCulling(true);
                }
                collectRanges(index);
                device->drawStaticRanges(levelData.geometry.id(), scratchFirsts.data(), scratchCounts.data(), static_cast<int32_t>(scratchFirsts.size()));
            }
            device->setColorWrite(true);
        }
        for (int32_t index : touchedBatches) {
            const Batch& batch = levelData.batches[static_cast<size_t>(index)];
            if (isTranslucentFlags(batch.polyFlags) || !(view.zoneMask & (1ull << (batch.zone & 63)))) {
                continue;
            }
            if (mirrorsActive && (batch.polyFlags & PF_Mirrored)) {
                continue;
            }
            collectRanges(index);
            drawBatch(batch, view, scratchFirsts.data(), scratchCounts.data(), static_cast<int32_t>(scratchFirsts.size()), -1, uploadedLights);
        }
        device->setPan(0, 0);
        device->setScale(1, 1);
        device->setBlend(BlendMode::Opaque);
    }

    void SceneRenderer::drawBatch(const Batch& batch, const View& view, const int32_t* firsts, const int32_t* counts, int32_t ranges, int32_t onlyNode, UploadedLights& uploaded)
    {
        bool backdropMask = (batch.polyFlags & PF_FakeBackdrop) && view.sky;
        UTexture* matrix = matrixTexture(sceneWorld ? sceneWorld->player() : nullptr);
        UTexture* texture = device->animated(matrix ? matrix : batch.texture);
        bool flat = renderMode == RenderMode::ZonePortal || renderMode == RenderMode::BspCuts;
        uint32_t geometry = flat ? flatVao() : levelData.geometry.id();
        const GLTextureInfo* info = device->texture(texture);
        uint32_t textureId = info && !flat && renderMode != RenderMode::LightingOnly ? info->id() : device->whiteTexture();
        int32_t extra = 0;
        bool batchUnlit = flat || (batch.polyFlags & (PF_Unlit | PF_Modulated)) != 0;
        std::vector<uint16_t>& lit = scratchLitLights;
        lit.clear();
        if (!batchUnlit) {
            litLights(batch.boundsMin, batch.boundsMax, (batch.polyFlags & PF_SpecialLit) != 0, lit);
        }
        if (!lit.empty()) {
            extra |= SHADER_Dynamic;
            if (!uploaded.valid || lit != uploaded.lights) {
                setLights(lit);
                uploaded.valid = true;
                uploaded.lights = lit;
            }
        }
        if (batch.lightmapPage >= 0 && !flat) {
            extra |= SHADER_Lightmap;
            device->bindLightmap(lightmaps.texture(batch.lightmapPage));
        }
        device->setOcclusionSource(batchUnlit ? OcclusionSource::None : OcclusionSource::World);
        applyBlend(flat ? batch.polyFlags & PF_TwoSided : batch.polyFlags, extra);
        float panU = 0;
        float panV = 0;
        float levelTime = static_cast<float>(time);
        if (batch.polyFlags & (PF_AutoUPan | PF_AutoVPan)) {
            UObject* zone = zoneActor(batch.zone);
            if (zone && zone->isA(nameZoneInfo)) {
                if (batch.polyFlags & PF_AutoUPan) {
                    panU += autoPanTexels(levelTime, zoneinfo::TexUPanSpeed(zone));
                }
                if (batch.polyFlags & PF_AutoVPan) {
                    panV += autoPanTexels(levelTime, zoneinfo::TexVPanSpeed(zone));
                }
            }
        }
        if (batch.polyFlags & PF_SmallWavy) {
            panU += smallWavyPanU(levelTime);
            panV += smallWavyPanV(levelTime);
        }
        float uSize = batch.uSize;
        float vSize = batch.vSize;
        if (matrix && info) {
            float matrixScale = texture->data && texture::DrawScale(texture) > 0 ? texture::DrawScale(texture) : 1.0f;
            uSize = static_cast<float>(info->width) * matrixScale;
            vSize = static_cast<float>(info->height) * matrixScale;
        }
        device->setScale(batch.uSize / uSize, batch.vSize / vSize);
        device->setPan(panU / uSize, panV / vSize);
        device->bindTexture(textureId);
        if (backdropMask) {
            device->setBlend(BlendMode::Opaque);
            device->setFlags(0);
            device->setColorWrite(false);
            device->drawStaticRanges(geometry, firsts, counts, ranges);
            device->setColorWrite(true);
            return;
        }
        if ((batch.polyFlags & PF_Translucent) && (extra & SHADER_Lightmap) && renderMode != RenderMode::Unlit) {
            applyBlend(batch.polyFlags, 0);
            device->drawStaticRanges(geometry, firsts, counts, ranges);
            device->setBlend(BlendMode::Modulated);
            device->setFlags(extra);
            device->setTint(0.5f, 0.5f, 0.5f, 1);
            device->drawStaticRanges(geometry, firsts, counts, ranges);
            device->setTint(1, 1, 1, 1);
        } else {
            device->drawStaticRanges(geometry, firsts, counts, ranges);
        }
        device->setScale(1, 1);
        if (renderMode == RenderMode::Textured) {
            drawFog(batch, view, onlyNode);
            drawDetail(batch, view, onlyNode);
        }
    }

    void SceneRenderer::drawDetail(const Batch& batch, const View& view, int32_t onlyNode)
    {
        if (!batch.detail || batch.detailPolys.empty() || !levelData.detailGeometry) {
            return;
        }
        const Vector& forward = view.axes.xAxis;
        Vector nearest { forward.x > 0 ? batch.boundsMin.x : batch.boundsMax.x, forward.y > 0 ? batch.boundsMin.y : batch.boundsMax.y, forward.z > 0 ? batch.boundsMin.z : batch.boundsMax.z };
        if ((nearest - view.location).dot(forward) >= options.detailDistance) {
            return;
        }
        const GLTextureInfo* info = device->texture(device->animated(batch.detail));
        if (!info) {
            return;
        }
        bool started = false;
        for (const auto& [first, count, surfIndex, node] : batch.detailPolys) {
            if ((onlyNode >= 0 && node != onlyNode) || levelData.nodeStamp[static_cast<size_t>(node)] != visibleStamp) {
                continue;
            }
            if (batch.fogZone && view.fog && static_cast<size_t>(surfIndex) < levelData.fogFrame.size() && levelData.fogFrame[static_cast<size_t>(surfIndex)] == fogStamp && levelData.fogged[static_cast<size_t>(surfIndex)]) {
                continue;
            }
            bool near = false;
            for (int32_t i = first; i < first + count; i++) {
                if ((levelData.detailPoints[static_cast<size_t>(i)] - view.location).dot(forward) < options.detailDistance) {
                    near = true;
                    break;
                }
            }
            if (!near) {
                continue;
            }
            if (!started) {
                device->setBlend(BlendMode::Modulated);
                device->setFlags(SHADER_Detail);
                device->setPan(0, 0);
                device->setEye(view.location, forward);
                device->bindTexture(info->id());
                device->setPolygonOffset(true, -1.0f, -1.0f);
                started = true;
            }
            device->drawStatic(levelData.detailGeometry.id(), first, count);
        }
        if (started) {
            device->setPolygonOffset(false, 0.0f, 0.0f);
        }
    }
}

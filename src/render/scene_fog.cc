#include "render/scene_renderer.h"

#include "engine/props_engine.h"
#include "render/lighting.h"
#include "render/scene_renderer_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace dx {
    void SceneRenderer::collectVolumetrics(const View& view)
    {
        volumetrics.clear();
        volumetricPalettes.clear();
        volumetricColors.clear();
        fogStamp++;
        volumetricSignature = 1469598103934665603ull;
        auto mix = [&](float value) {
            uint32_t bits = std::bit_cast<uint32_t>(value);
            volumetricSignature = (volumetricSignature ^ bits) * 1099511628211ull;
        };
        std::vector<UObject*> found;
        auto outside = [&](const Vector& location, float radius) {
            return sphereOutsideView(view, location, radius);
        };
        auto add = [&](UObject* light) {
            float brightness = 0;
            Vector color = lightColor(light, static_cast<float>(time), &brightness);
            volumetrics.push_back(prepareVolumetric(actor::Location(light) - view.location, actor::VolumeRadius(light), brightness, actor::VolumeBrightness(light)));
            volumetricPalettes.push_back({ static_cast<int64_t>(std::floor(color.x * 65536.0f)), static_cast<int64_t>(std::floor(color.y * 65536.0f)), static_cast<int64_t>(std::floor(color.z * 65536.0f)) });
            volumetricColors.push_back(color);
            const Vector& location = actor::Location(light);
            mix(location.x);
            mix(location.y);
            mix(location.z);
            mix(volumetrics.back().radius);
            mix(volumetrics.back().brightness);
            mix(color.x);
            mix(color.y);
            mix(color.z);
            found.push_back(light);
        };
        std::vector<UObject*> dynamicVolumetrics;
        std::vector<int32_t> dynamicLeaves;
        for (UObject* light : lights) {
            if (((actor::bStatic(light) || actor::bNoDelete(light)) && !actor::bDynamicLight(light)) || actor::LightRadius(light) == 0) {
                continue;
            }
            if (!actor::VolumeRadius(light) || !actor::VolumeBrightness(light)) {
                continue;
            }
            UObject* zone = zoneForActor(light);
            if (!zone || !zone->data || !zone->isA(nameZoneInfo) || !zoneinfo::bFogZone(zone)) {
                continue;
            }
            float radius = 25.0f * (static_cast<float>(actor::VolumeRadius(light)) + 1.0f);
            if (outside(actor::Location(light), radius) || outside(actor::Location(light), std::max(radius, lightRadius(light)))) {
                continue;
            }
            dynamicVolumetrics.push_back(light);
            dynamicLeaves.push_back(pointLeaf(actor::Location(light)));
        }
        UModel* model = levelData.model;
        if (!model || model->nodes.empty() || !view.visibility || (!levelData.anyVolumetric && dynamicVolumetrics.empty())) {
            return;
        }
        auto visitLeaf = [&](int32_t leafIndex) {
            if (leafIndex < 0 || static_cast<size_t>(leafIndex) >= model->leaves.size()) {
                return;
            }
            const ConvexLeaf& leaf = model->leaves[static_cast<size_t>(leafIndex)];
            if (!(view.zoneMask & (1ull << (leaf.zone & 63)))) {
                return;
            }
            if (leaf.volumetric >= 0) {
                for (size_t i = static_cast<size_t>(leaf.volumetric); i < model->lights.size() && model->lights[i]; i++) {
                    UObject* light = model->lights[i];
                    if (!light->data || light->destroyed || std::find(found.begin(), found.end(), light) != found.end()) {
                        continue;
                    }
                    float radius = 25.0f * (static_cast<float>(actor::VolumeRadius(light)) + 1.0f);
                    if ((actor::Location(light) - view.location).sizeSquared() > radius * radius && outside(actor::Location(light), radius)) {
                        continue;
                    }
                    add(light);
                }
            }
            for (size_t i = 0; i < dynamicVolumetrics.size(); i++) {
                if (dynamicLeaves[i] == leafIndex && std::find(found.begin(), found.end(), dynamicVolumetrics[i]) == found.end()) {
                    add(dynamicVolumetrics[i]);
                }
            }
            levelData.leafVolumetrics[static_cast<size_t>(leafIndex)] = static_cast<int32_t>(volumetrics.size());
        };
        const ViewVisibility& visibility = *view.visibility;
        size_t nextLeaf = 0;
        for (size_t i = 0; i <= visibility.nodes.size(); i++) {
            int32_t sequence = i < visibility.nodes.size() ? visibility.nodeSequence[i] : std::numeric_limits<int32_t>::max();
            while (nextLeaf < visibility.leaves.size() && visibility.leaves[nextLeaf].second < sequence) {
                visitLeaf(visibility.leaves[nextLeaf++].first);
            }
            if (i < visibility.nodes.size()) {
                levelData.nodeVolumetrics[static_cast<size_t>(visibility.nodes[i])] = static_cast<int32_t>(volumetrics.size());
            }
        }
        mix(static_cast<float>(volumetrics.size()));
    }

    Vector SceneRenderer::meshFog(const Vector& point, const View& view, size_t count) const
    {
        Vector fog;
        for (size_t i = 0; i < count; i++) {
            float value = 2.0f * volumetricFog(volumetrics[i], point - view.location);
            if (i > 0 && value == 0) {
                continue;
            }
            Vector local = volumetricColors[i] * value;
            local = { std::min(local.x, 1.0f), std::min(local.y, 1.0f), std::min(local.z, 1.0f) };
            if (i == 0) {
                fog = local;
            } else {
                fog = fog + local - local * fog;
            }
        }
        return fog;
    }

    bool SceneRenderer::surfaceFog(int32_t surfIndex, const View& view)
    {
        size_t index = static_cast<size_t>(surfIndex);
        if (levelData.fogFrame[index] == fogStamp) {
            return levelData.fogged[index] != 0;
        }
        levelData.fogFrame[index] = fogStamp;
        levelData.fogged[index] = 0;
        FogCache& cache = levelData.fogCache[index];
        int32_t count = 0;
        for (int32_t node : levelData.surfNodes[index]) {
            if (levelData.nodeStamp[static_cast<size_t>(node)] == visibleStamp) {
                count = std::max(count, levelData.nodeVolumetrics[static_cast<size_t>(node)]);
            }
        }
        if (cache.valid && cache.signature == volumetricSignature && cache.count == count && sameVector(cache.eye, view.location)) {
            levelData.fogged[index] = cache.fogged ? 1 : 0;
            return cache.fogged;
        }
        cache.valid = true;
        cache.fogged = false;
        cache.signature = volumetricSignature;
        cache.eye = view.location;
        cache.count = count;
        UModel* model = levelData.model;
        const BspSurf& surf = model->surfs[index];
        if (surf.lightMap < 0 || static_cast<size_t>(surf.lightMap) >= model->lightMaps.size()) {
            return false;
        }
        const LightMapIndex& lightMap = model->lightMaps[static_cast<size_t>(surf.lightMap)];
        int32_t width = std::max(lightMap.uClamp, 1);
        int32_t height = std::max(lightMap.vClamp, 1);
        std::vector<Vector>& points = levelData.fogPoints[surfIndex];
        if (points.empty()) {
            points.resize(static_cast<size_t>(width * height));
            for (int32_t y = 0; y < height; y++) {
                for (int32_t x = 0; x < width; x++) {
                    points[static_cast<size_t>(y * width + x)] = lightmapTexelPoint(model, surf, x, y);
                }
            }
        }
        if (cache.extent < 0) {
            Vector low = points[0];
            Vector high = points[0];
            for (const Vector& point : points) {
                low = { std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z) };
                high = { std::max(high.x, point.x), std::max(high.y, point.y), std::max(high.z, point.z) };
            }
            cache.center = (low + high) * 0.5f;
            cache.extent = (high - low).size() * 0.5f;
        }
        std::vector<uint8_t>& bytes = scratchFog;
        bytes.assign(points.size() * 3, 0);
        bool any = false;
        for (size_t light = 0; light < static_cast<size_t>(count); light++) {
            const VolumetricLight& info = volumetrics[light];
            const std::array<int64_t, 3>& palette = volumetricPalettes[light];
            Vector toCenter = cache.center - view.location;
            float along = std::clamp(info.location.dot(toCenter) / std::max(toCenter.sizeSquared(), 1e-6f), 0.0f, 1.0f);
            if ((info.location - toCenter * along).size() - cache.extent > info.radius) {
                continue;
            }
            for (size_t i = 0; i < points.size(); i++) {
                float value = volumetricFog(info, points[i] - view.location);
                int64_t level = static_cast<int64_t>(std::nearbyint(value * 255.0f));
                if (level <= 0) {
                    continue;
                }
                for (size_t c = 0; c < 3; c++) {
                    uint8_t add = static_cast<uint8_t>(std::min<int64_t>((level * palette[c]) >> 16, 127));
                    bytes[i * 3 + c] = fogMerge(bytes[i * 3 + c], add);
                    any = any || bytes[i * 3 + c] != 0;
                }
            }
        }
        if (!any) {
            return false;
        }
        lightmaps.uploadFog(surf, width, height, bytes);
        levelData.fogged[index] = 1;
        cache.fogged = true;
        return true;
    }

    void SceneRenderer::drawFog(const Batch& batch, const View& view, int32_t onlyNode)
    {
        if (!view.fog || !batch.fogZone || volumetrics.empty() || batch.lightmapPage < 0) {
            return;
        }
        bool started = false;
        for (const auto& [first, count, surfIndex, backFog, frontFog, node] : batch.surfPolys) {
            if ((onlyNode >= 0 && node != onlyNode) || levelData.nodeStamp[static_cast<size_t>(node)] != visibleStamp) {
                continue;
            }
            const BspSurf& surf = levelData.model->surfs[static_cast<size_t>(surfIndex)];
            const Vector& normal = levelData.model->vectors[static_cast<size_t>(surf.vNormal)];
            bool front = normal.dot(view.location - levelData.model->points[static_cast<size_t>(surf.pBase)]) > 0;
            if (!(front ? frontFog : backFog) || !surfaceFog(surfIndex, view)) {
                continue;
            }
            if (!started) {
                device->setBlend(BlendMode::Highlight);
                device->setFlags(SHADER_Fog);
                device->bindLightmap(lightmaps.fogTexture(batch.lightmapPage));
                if (batch.polyFlags & PF_Masked) {
                    device->setDepthFunc(DepthFunc::Equal);
                }
                started = true;
            }
            device->drawStatic(levelData.geometry.id(), first, count);
        }
        if (started) {
            device->setDepthFunc(DepthFunc::LessEqual);
        }
    }
}

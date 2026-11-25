#include "render/scene_renderer.h"

#include "core/profile.h"
#include "engine/props_engine.h"
#include "render/scene_renderer_internal.h"

#include <algorithm>

namespace dx {
    void SceneRenderer::setActorOrder(UObject* actor, const View& view)
    {
        UModel* model = levelData.model;
        Vector location = actor::Location(actor);
        currentOrder = fallbackOrder;
        int32_t index = 0;
        for (int guard = 0; model && guard < 10000 && static_cast<size_t>(index) < model->nodes.size(); guard++) {
            if (levelData.cullStamp[static_cast<size_t>(index)] == visibleStamp) {
                currentOrder = levelData.cullOrder[static_cast<size_t>(index)];
                break;
            }
            const BspNode& node = model->nodes[static_cast<size_t>(index)];
            bool front = node.plane.x * location.x + node.plane.y * location.y + node.plane.z * location.z - node.plane.w >= 0;
            int32_t next = front ? node.front : node.back;
            if (next <= 0) {
                int32_t leaf = node.leaf[front ? 1 : 0];
                if (leaf >= 0 && static_cast<size_t>(leaf) < leafOrder.size() && levelData.leafStamp[static_cast<size_t>(leaf)] == visibleStamp) {
                    currentOrder = leafOrder[static_cast<size_t>(leaf)];
                }
                break;
            }
            index = next;
        }
        currentDistance = (actor::Location(actor) - view.location).size();
    }

    void SceneRenderer::queueTranslucent(uint32_t flags, uint32_t texture, const GroupExtra& extra, const std::vector<GLVertex>& vertices)
    {
        if (translucentCount == translucentItems.size()) {
            translucentItems.emplace_back();
        }
        TranslucentItem& item = translucentItems[translucentCount++];
        item.order = currentOrder;
        item.distance = currentDistance;
        item.flags = flags;
        item.texture = texture;
        item.extra = extra;
        item.vertices.assign(vertices.begin(), vertices.end());
    }

    void SceneRenderer::sortTranslucent()
    {
        std::vector<size_t>& order = scratchTranslucentOrder;
        order.resize(translucentCount);
        for (size_t i = 0; i < translucentCount; i++) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const TranslucentItem& first = translucentItems[a];
            const TranslucentItem& second = translucentItems[b];
            if (first.order != second.order) {
                return first.order < second.order;
            }
            return first.distance > second.distance;
        });
    }

    void SceneRenderer::drawTranslucentItem(size_t index)
    {
        const TranslucentItem& item = translucentItems[index];
        drawGroup(item.flags, item.texture, item.extra, item.vertices);
    }

    // Translucent actors alone, for views without level surfaces (wireframe, overlays); the level
    // views draw them sorted in with the translucent surfaces in drawLevelGeometry.
    void SceneRenderer::flushTranslucent(const View& view)
    {
        applyView(view);
        sortTranslucent();
        for (size_t index : scratchTranslucentOrder) {
            drawTranslucentItem(index);
        }
        translucentCount = 0;
        device->setBlend(BlendMode::Opaque);
    }

    void SceneRenderer::flushMeshBatches()
    {
        DX_TIME("Mesh batch flush");
        flushBatches(meshBatches);
    }

    void SceneRenderer::drawGroup(uint32_t flags, uint32_t texture, const GroupExtra& extra, const std::vector<GLVertex>& vertices)
    {
        if (vertices.empty()) {
            return;
        }
        bool reflecting = extra.mirror && moverMirrorsActive(drawRecursion);
        if (reflecting && !isTranslucentFlags(flags)) {
            return;
        }

        // Behind a level surface on the same plane (EmittedGroup::brush).
        struct PushBack {
            GLRenderDevice* device;
            bool on;

            ~PushBack()
            {
                if (on) {
                    device->setPolygonOffset(false, 0.0f, 0.0f);
                }
            }
        } pushBack { device.get(), extra.brush };

        if (extra.brush) {
            device->setPolygonOffset(true, 1.0f, 4.0f);
        }
        if (!extra.lightmap || renderMode == RenderMode::Unlit) {
            applyBlend(flags, 0);
            device->setCulling(false);
            device->bindTexture(renderMode == RenderMode::LightingOnly ? device->whiteTexture() : texture);
            device->draw(vertices, Primitive::Triangles);
            return;
        }
        int32_t shaderFlags = SHADER_Lightmap;
        if (extra.lightMask) {
            shaderFlags |= SHADER_Dynamic;
            batchLights.clear();
            for (size_t i = 0; i < dynamicLights.size(); i++) {
                if (extra.lightMask & (1u << i)) {
                    batchLights.push_back(dynamicLights[i]);
                }
            }
            device->setDynamicLights(batchLights);
        }
        device->bindLightmap(extra.lightmap);
        device->bindTexture(renderMode == RenderMode::LightingOnly ? device->whiteTexture() : texture);
        // Texture and lightmap in one pass with the surface's blend, translucent ones included,
        // as D3DDrv's DrawComplexSurface draws them with multitexture.
        applyBlend(flags, shaderFlags);
        if (extra.cull) {
            device->setCulling(true);
        } else {
            device->setCulling(false);
        }
        device->draw(vertices, Primitive::Triangles);
    }

    void SceneRenderer::flushBatches(std::unordered_map<uint64_t, std::vector<GLVertex>>& batches)
    {
        for (auto& [key, vertices] : batches) {
            drawGroup(meshBatchFlags[key], meshBatchTextures[key], meshBatchExtras[key], vertices);
            vertices.clear();
        }
        device->setBlend(BlendMode::Opaque);
    }

    void SceneRenderer::appendGroups(const std::vector<EmittedGroup>& groups, std::unordered_map<uint64_t, std::vector<GLVertex>>& opaque)
    {
        DX_TIME("Mesh group copy");
        for (const EmittedGroup& group : groups) {
            if (group.translucent) {
                queueTranslucent(group.flags, group.texture, { group.lightmap, group.lightMask, group.cull, group.brush, group.mirror }, group.vertices);
                continue;
            }
            meshBatchFlags[group.key] = group.flags;
            meshBatchTextures[group.key] = group.texture;
            meshBatchExtras[group.key] = { group.lightmap, group.lightMask, group.cull, group.brush, group.mirror };
            std::vector<GLVertex>& out = opaque[group.key];
            out.insert(out.end(), group.vertices.begin(), group.vertices.end());
        }
    }
}

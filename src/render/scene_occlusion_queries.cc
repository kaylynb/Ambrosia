#include "render/scene_renderer.h"

#include "core/objects.h"
#include "core/profile.h"
#include "engine/mesh.h"
#include "engine/props_engine.h"
#include "render/scene_renderer_internal.h"

#include <cmath>

namespace dx {
    bool SceneRenderer::actorBounds(UObject* actor, Vector& center, float& radius) const
    {
        UObject* meshObject = actor::Mesh(actor);
        if (!meshObject) {
            return false;
        }
        UMesh* mesh = cast<UMesh>(meshObject);
        auto found = meshDrawCaches.find({ actor, mesh, actor });
        if (found == meshDrawCaches.end() || !found->second.valid) {
            return false;
        }
        const MeshDrawCache& cache = found->second;
        if (cache.world.size() < 64) {
            return false;
        }
        float scale = std::abs(actor::DrawScale(actor));
        float cachedScale = cache.transform.x.size();
        if (cachedScale <= 0 || std::abs(scale - cachedScale) > cachedScale * 0.01f) {
            return false;
        }
        center = actor::Location(actor) + actor::PrePivot(actor);
        radius = cache.sphereRadius + (cache.sphereCenter - cache.transform.origin).size();
        return true;
    }

    void SceneRenderer::cullOccluded(std::vector<UObject*>& candidates, const View& view)
    {
        occlusionBoxes.clear();
        occlusionIssued.clear();
        occlusionUnconfirmed.clear();
        size_t kept = 0;
        for (UObject* actor : candidates) {
            bool keep = true;
            Vector center;
            float radius = 0;
            if (actor::DrawType(actor) == DT_Mesh && !actorBounds(actor, center, radius)) {
                occlusionUnconfirmed.insert(actor);
            } else if (actor::DrawType(actor) == DT_Mesh) {
                float extent = radius + 48.0f;
                Vector delta = view.location - center;
                float near = extent + 32.0f;
                bool inside = std::abs(delta.x) < near && std::abs(delta.y) < near && std::abs(delta.z) < near;
                if (!inside) {
                    OcclusionState& state = occlusion[actor];
                    if (state.pending) {
                        DX_TIME("Occlusion: poll results");
                        bool visible = false;
                        if (device->queryResult(state.query.get(), visible)) {
                            state.pending = false;
                            state.known = true;
                            state.visible = visible;
                            state.resultFrame = state.issuedFrame;
                        }
                    }
                    keep = !state.known || state.visible || sceneFrame - state.resultFrame > 2;
                    if (keep && !(state.known && state.visible)) {
                        occlusionUnconfirmed.insert(actor);
                    }
                    if (!state.pending) {
                        if (!state.query) {
                            state.query = device->createQuery();
                        }
                        Vector low = center - Vector { extent, extent, extent };
                        Vector high = center + Vector { extent, extent, extent };
                        Vector corners[8];
                        for (int c = 0; c < 8; c++) {
                            corners[c] = { (c & 1) ? high.x : low.x, (c & 2) ? high.y : low.y, (c & 4) ? high.z : low.z };
                        }
                        static const int faces[36] = { 0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3 };
                        for (int index : faces) {
                            GLVertex vertex;
                            vertex.x = corners[index].x;
                            vertex.y = corners[index].y;
                            vertex.z = corners[index].z;
                            occlusionBoxes.push_back(vertex);
                        }
                        state.issuedFrame = sceneFrame;
                        state.pending = true;
                        occlusionIssued.push_back(&state);
                    }
                }
            }
            if (keep) {
                candidates[kept++] = actor;
            }
        }
        candidates.resize(kept);
        if (occlusionIssued.empty()) {
            return;
        }
        DX_TIME("Occlusion: issue queries");
        device->setBlend(BlendMode::Opaque);
        device->setFlags(0);
        device->setDepthWrite(false);
        device->setColorWrite(false);
        device->setCulling(false);
        device->upload(occlusionBoxes);
        for (size_t i = 0; i < occlusionIssued.size(); i++) {
            device->beginQuery(occlusionIssued[i]->query.get());
            device->drawRange(static_cast<int32_t>(i * 36), 36);
            device->endQuery();
        }
        device->setColorWrite(true);
        device->setDepthWrite(true);
    }
}

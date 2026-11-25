#include "render/scene_renderer.h"

#include "core/log.h"
#include "engine/mesh.h"
#include "render/scene_renderer_internal.h"

#include <algorithm>
#include <utility>

namespace dx {
    namespace {
        // Finds whether an object's own pointers lead to any of the objects going.
        class FindReleased final : public ReferenceVisitor {
        public:
            explicit FindReleased(const gc::ReleasedSet& set)
                : released(set)
            {
            }

            void visit(UObject*& reference) override
            {
                found = found || (reference && released.count(reference));
            }

            const gc::ReleasedSet& released;
            bool found = false;
        };

        bool modelHolds(UModel& model, const gc::ReleasedSet& released)
        {
            FindReleased finder(released);
            model.addReferences(finder);
            return finder.found;
        }
    }

    void SceneRenderer::objectsReleased(const gc::ReleasedSet& released)
    {
        if (drawing) {
            dx::log::warn("DevRender", "objects were collected in the middle of a scene draw");
        }
        auto gone = [&](const UObject* object) {
            return object && released.count(object) != 0;
        };
        auto lightsGone = [&](const LightState& state) {
            return std::any_of(state.actors.begin(), state.actors.end(), gone);
        };

        // The level's built geometry keeps the level, its model, its LevelInfo and the textures
        // of its surfaces. Any of them going makes it all stale; the next draw call or frame
        // flushes it, and until then nothing reads these pointers.
        bool levelGone = gone(levelData.level) || gone(levelData.model) || gone(levelData.levelInfo);
        for (const Batch& batch : levelData.batches) {
            levelGone = levelGone || gone(batch.texture) || gone(batch.detail);
        }
        if (levelGone) {
            levelReleased = true;
            levelData.level = nullptr;
            levelData.model = nullptr;
            levelData.levelInfo = nullptr;
            for (Batch& batch : levelData.batches) {
                batch.texture = nullptr;
                batch.detail = nullptr;
            }
        }
        lightmaps.objectsReleased(released);

        // The lights of the last drawSceneRect are read again by drawActorOverlay.
        size_t keptLights = 0;
        for (size_t i = 0; i < lights.size(); i++) {
            if (gone(lights[i])) {
                continue;
            }
            lights[keptLights] = lights[i];
            lightPrepared[keptLights] = lightPrepared[i];
            lightColors[keptLights] = lightColors[i];
            keptLights++;
        }
        if (keptLights != lights.size()) {
            lights.resize(keptLights);
            lightPrepared.resize(keptLights);
            lightColors.resize(keptLights);
            lightIndex.clear();
            for (size_t i = 0; i < lights.size(); i++) {
                lightIndex[lights[i]] = i;
            }
        }

        // Scratch: each is cleared or assigned before it is read in the call that uses it, so
        // emptying it only stops it pointing at freed objects in between.
        actorLights.clear();
        actorLightPrepared.clear();
        actorLightColors.clear();
        lightConsider.clear();
        scratchSkins.clear();
        scratchCandidates.clear();
        scratchDecals.clear();
        occlusionIssued.clear();
        std::erase_if(occlusionUnconfirmed, gone);

        std::erase_if(actorLightCache, [&](auto& entry) {
            if (gone(entry.first)) {
                return true;
            }
            std::erase_if(entry.second, [&](const LightSender& sender) {
                return gone(sender.light);
            });
            return false;
        });
        for (CoronaLight& corona : coronas) {
            if (gone(corona.actor)) {
                corona = CoronaLight {};
            }
        }

        std::erase_if(triangleCache, [&](const auto& entry) {
            return gone(entry.first);
        });
        std::erase_if(poseCaches, [&](const auto& entry) {
            const MeshPoseCaches& caches = entry.second;
            bool blendGone = std::any_of(std::begin(caches.blends), std::end(caches.blends), [&](const MeshPoseCache& blend) {
                return gone(blend.mesh);
            });
            return gone(entry.first.first) || gone(entry.first.second) || gone(caches.main.mesh) || blendGone;
        });
        std::erase_if(meshDrawCaches, [&](auto& entry) {
            if (gone(std::get<0>(entry.first)) || gone(std::get<1>(entry.first)) || gone(std::get<2>(entry.first))) {
                return true;
            }
            // The lights it was lit by are compared by address on the next draw.
            if (lightsGone(entry.second.lights)) {
                entry.second.cacheable = false;
                entry.second.lights = {};
            }
            return false;
        });
        for (auto it = brushDrawCaches.begin(); it != brushDrawCaches.end();) {
            BrushDrawCache& cache = it->second;
            bool drop = gone(it->first) || gone(cache.brush) || gone(cache.textureOverride) || lightsGone(cache.lights) || (cache.frameModel && modelHolds(*cache.frameModel, released));
            if (!drop) {
                if (cache.atlas) {
                    cache.atlas->objectsReleased(released);
                }
                ++it;
                continue;
            }
            // Its lightmap textures are deleted with it, after the frame.
            retiredBrushes.push_back(std::move(cache));
            it = brushDrawCaches.erase(it);
        }
        std::erase_if(occlusion, [&](auto& entry) {
            if (!gone(entry.first)) {
                return false;
            }
            if (entry.second.query) {
                retiredQueries.push_back(std::move(entry.second.query));
            }
            return true;
        });
    }

    void SceneRenderer::dropRetired()
    {
        retiredQueries.clear();
        retiredBrushes.clear();
    }

    void SceneRenderer::settleReleased()
    {
        if (levelReleased) {
            flushLevel();
            return;
        }
        dropRetired();
    }

    bool SceneRenderer::holds(const UObject* object) const
    {
        if (!object) {
            return false;
        }
        auto has = [&](const std::vector<UObject*>& list) {
            return std::find(list.begin(), list.end(), object) != list.end();
        };
        if (levelData.level == object || levelData.model == object || levelData.levelInfo == object || has(lights) || has(actorLights) || has(lightConsider) || has(scratchCandidates) || has(scratchDecals)) {
            return true;
        }
        for (const Batch& batch : levelData.batches) {
            if (batch.texture == object || batch.detail == object) {
                return true;
            }
        }
        if (lightIndex.count(const_cast<UObject*>(object)) || occlusionUnconfirmed.count(const_cast<UObject*>(object)) || occlusion.count(const_cast<UObject*>(object)) || lightmaps.holdsLight(object)) {
            return true;
        }
        if (std::find(scratchSkins.begin(), scratchSkins.end(), object) != scratchSkins.end()) {
            return true;
        }
        for (const auto& [actor, senders] : actorLightCache) {
            if (actor == object || std::any_of(senders.begin(), senders.end(), [&](const LightSender& sender) { return sender.light == object; })) {
                return true;
            }
        }
        for (const CoronaLight& corona : coronas) {
            if (corona.actor == object) {
                return true;
            }
        }
        for (const auto& [mesh, triangles] : triangleCache) {
            if (mesh == object) {
                return true;
            }
        }
        for (const auto& [key, caches] : poseCaches) {
            if (key.first == object || key.second == object || caches.main.mesh == object) {
                return true;
            }
        }
        for (const auto& [key, cache] : meshDrawCaches) {
            if (std::get<0>(key) == object || std::get<1>(key) == object || std::get<2>(key) == object || has(cache.lights.actors)) {
                return true;
            }
        }
        for (const auto& [actor, cache] : brushDrawCaches) {
            if (actor == object || cache.brush == object || cache.textureOverride == object || has(cache.lights.actors) || (cache.atlas && cache.atlas->holdsLight(object))) {
                return true;
            }
            if (cache.frameModel) {
                gc::ReleasedSet one { object };
                if (modelHolds(*cache.frameModel, one)) {
                    return true;
                }
            }
        }
        return device->holdsTexture(object);
    }
}

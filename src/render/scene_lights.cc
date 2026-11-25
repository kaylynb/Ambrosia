#include "render/scene_renderer.h"

#include "engine/props_engine.h"
#include "engine/world.h"
#include "render/lighting.h"
#include "render/scene_renderer_internal.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dx {
    namespace {
        bool samePrepared(const PreparedLight& a, const PreparedLight& b)
        {
            return sameVector(a.location, b.location) && a.radius == b.radius && a.effect == b.effect && sameVector(a.spotDirection, b.spotDirection) && a.outer == b.outer;
        }
    }

    uint32_t SceneRenderer::litMask(const Vector& low, const Vector& high, bool specialLit) const
    {
        uint32_t mask = 0;
        for (size_t i = 0; i < dynamicLights.size(); i++) {
            const DynamicLight& light = dynamicLights[i];
            if (light.specialLit != specialLit) {
                continue;
            }
            Vector closest { std::clamp(light.location.x, low.x, high.x), std::clamp(light.location.y, low.y, high.y), std::clamp(light.location.z, low.z, high.z) };
            if ((closest - light.location).sizeSquared() < light.radius * light.radius) {
                mask |= 1u << i;
            }
        }
        return mask;
    }

    void SceneRenderer::selectLights(const Vector& location, float extent)
    {
        actorLights.clear();
        actorLightPrepared.clear();
        actorLightColors.clear();
        for (size_t i = 0; i < lights.size(); i++) {
            const PreparedLight& prepared = lightPrepared[i];
            float reach = prepared.radius + extent;
            if ((prepared.location - location).sizeSquared() < reach * reach) {
                actorLights.push_back(lights[i]);
                actorLightPrepared.push_back(prepared);
                actorLightColors.push_back(lightColors[i]);
            }
        }
    }

    void SceneRenderer::selectActorLights(UObject* actor)
    {
        actorLights.clear();
        actorLightPrepared.clear();
        actorLightColors.clear();
        UModel* model = levelData.model;
        int32_t actorLeaf = *reinterpret_cast<int32_t*>(actor::Region(actor) + sizeof(UObject*));
        if (actor::bUnlit(actor) || !model || actorLeaf < 0 || model->leaves.empty() || static_cast<size_t>(actorLeaf) >= model->leaves.size()) {
            return;
        }
        auto cached = actorLightCache.find(actor);
        bool firstSee = cached == actorLightCache.end();
        std::vector<LightSender>& senders = actorLightCache[actor];
        senders.erase(std::remove_if(senders.begin(), senders.end(), [](const LightSender& sender) {
            return !sender.light || sender.light->destroyed;
        }),
            senders.end());
        lightConsider.clear();
        considerTag = static_cast<int32_t>(static_cast<uint32_t>(considerTag) + 1u);
        Vector actorLocation = actor::Location(actor);
        auto consider = [&](UObject* light, int32_t factor) {
            if (!light || light->destroyed || actor::ExtraTag(light) == considerTag) {
                return;
            }
            float distSquared = (actor::Location(light) - actorLocation).sizeSquared();
            float radius = 25.0f * (static_cast<float>(actor::LightRadius(light)) + 1.0f);
            if (((actor::bSpecialLit(actor) != 0) != (actor::bSpecialLit(light) != 0)) || radius * radius <= distSquared) {
                return;
            }
            float bright = static_cast<float>(actor::LightBrightness(light));
            if (actor::LightEffect(light) == 17) {
                bright = static_cast<float>(static_cast<double>(bright) * 0.75);
                radius = static_cast<float>(static_cast<double>(radius) * 0.75);
            }
            float tag = static_cast<float>((1.0 - static_cast<double>(std::sqrt(distSquared) / radius)) * static_cast<double>(bright) * 1024.0);
            actor::LightingTag(light) = static_cast<int32_t>(std::nearbyint(tag));
            actor::ExtraTag(light) = considerTag;
            actor::SpecialTag(light) = factor;
            lightConsider.push_back(light);
        };
        for (const LightSender& sender : senders) {
            consider(sender.light, sender.factor);
        }
        int32_t permeating = model->leaves[static_cast<size_t>(actorLeaf)].permeating;
        if (permeating >= 0) {
            for (size_t i = static_cast<size_t>(permeating); i < model->lights.size() && model->lights[i] && lightConsider.size() < 120; i++) {
                consider(model->lights[i], 0);
            }
        }
        for (UObject* light : lights) {
            if (lightConsider.size() >= 120) {
                break;
            }
            if (!actor::bStatic(light)) {
                consider(light, 255);
            }
        }
        std::sort(lightConsider.begin(), lightConsider.end(), [](UObject* a, UObject* b) {
            return actor::LightingTag(a) > actor::LightingTag(b);
        });
        std::vector<UObject*> accepted;
        int32_t numVisible = 0;
        int32_t numStaticVisible = 0;
        int32_t threshold = -1;
        World* world = sceneWorld;
        for (UObject* light : lightConsider) {
            bool isStatic = actor::bStatic(light) != 0;
            bool visible = false;
            if (actor::LightingTag(light) < threshold || (isStatic ? numStaticVisible : numVisible) >= 8) {
                visible = false;
            } else if (!isStatic && actor::bMovable(light)) {
                visible = true;
            } else if (firstSee || ((static_cast<int32_t>(light->handle) ^ lightFrameCount) & 0xf) == 0) {
                CheckResult hit;
                visible = !world || !world->bspLineCheck(hit, actorLocation, actor::Location(light), {});
                if (visible && firstSee) {
                    actor::SpecialTag(light) = 255;
                }
            } else {
                visible = (actor::SpecialTag(light) & 1) != 0;
            }
            if (visible) {
                numVisible++;
                if (isStatic) {
                    numStaticVisible++;
                }
                if (threshold == -1) {
                    threshold = actor::LightingTag(light) / 8;
                }
                accepted.push_back(light);
                actor::SpecialTag(light) = std::min(actor::SpecialTag(light) + lightDelta, 255) | 1;
            } else {
                actor::SpecialTag(light) = std::max(actor::SpecialTag(light) - lightDelta, 0) & ~1;
                if (actor::SpecialTag(light) > 0) {
                    accepted.push_back(light);
                }
            }
        }
        senders.clear();
        for (UObject* light : lightConsider) {
            if (senders.size() >= 16) {
                break;
            }
            if (actor::SpecialTag(light) > 1) {
                senders.push_back({ light, actor::SpecialTag(light) });
            }
        }
        for (UObject* light : accepted) {
            if (actor::LightType(light) == 0 || actor::LightBrightness(light) == 0 || light == actor || actorLights.size() >= 256) {
                continue;
            }
            auto found = lightIndex.find(light);
            PreparedLight prepared = found != lightIndex.end() ? lightPrepared[found->second] : prepareLight(light, actor::Location(light));
            Vector color = found != lightIndex.end() ? lightColors[found->second] : lightColor(light, static_cast<float>(time));
            float factor = static_cast<float>(static_cast<double>(actor::SpecialTag(light)) * (1.0 / 255.0));
            actorLights.push_back(light);
            actorLightPrepared.push_back(prepared);
            actorLightColors.push_back(color * factor);
        }
    }

    void SceneRenderer::collectLights(const std::vector<UObject*>& actors)
    {
        lights.clear();
        lightPrepared.clear();
        lightColors.clear();
        lightIndex.clear();
        for (UObject* actor : actors) {
            if (isLightSource(actor)) {
                lightIndex[actor] = lights.size();
                lights.push_back(actor);
                lightPrepared.push_back(prepareLight(actor, actor::Location(actor)));
                lightColors.push_back(lightColor(actor, static_cast<float>(time)));
            }
        }
    }

    void SceneRenderer::collectDynamicLights(const Vector& viewLocation)
    {
        std::vector<std::pair<float, UObject*>> candidates;
        for (UObject* light : lights) {
            if (((actor::bStatic(light) || actor::bNoDelete(light)) && !actor::bDynamicLight(light)) || actor::LightRadius(light) == 0) {
                continue;
            }
            float radius = lightRadius(light);
            float distance = (actor::Location(light) - viewLocation).size() - radius;
            candidates.emplace_back(distance, light);
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        dynamicLights.clear();
        for (const auto& [distance, light] : candidates) {
            if (dynamicLights.size() >= MaxDynamicLights) {
                break;
            }
            DynamicLight entry;
            entry.location = actor::Location(light);
            entry.radius = lightRadius(light);
            entry.color = lightColor(light, static_cast<float>(time));
            entry.specialLit = actor::bSpecialLit(light) != 0;
            uint8_t effect = actor::LightEffect(light);
            if (effect == 8 || effect == 12) {
                entry.direction = -rotationVector(actor::Rotation(light));
                entry.cone = 1.0f - static_cast<float>(actor::LightCone(light)) / 256.0f;
            }
            dynamicLights.push_back(entry);
        }
    }

    Vector SceneRenderer::actorLight(UObject* actor, const Vector& point, const Vector& normal, const Vector& ambient, float scaleGlow, bool unlit, const Vector* eye)
    {
        if (unlit) {
            return { 1, 1, 1 };
        }
        Vector lit;
        for (size_t i = 0; i < actorLights.size(); i++) {
            UObject* light = actorLights[i];
            if (light == actor) {
                continue;
            }
            const PreparedLight& prepared = actorLightPrepared[i];
            Vector delta = prepared.location - point;
            float distance = delta.size();
            if (distance >= prepared.radius || distance <= 0) {
                continue;
            }
            float incidence = 1.0f + delta.dot(normal) / distance;
            float g = std::max(incidence * incidence - 1.5f, 0.0f);
            if (eye) {
                Vector toPoint = point - *eye;
                Vector mirrored = prepared.location - normal * (2.0f * normal.dot(prepared.location - point));
                float specular = (mirrored - point).dot(toPoint);
                float pointSquared = toPoint.sizeSquared();
                if (specular > 0 && pointSquared > 0) {
                    g += 6.0f * specular * specular / (distance * distance * pointSquared);
                }
            }
            g *= 1.0f - distance / prepared.radius;
            if (g > 0) {
                lit += actorLightColors[i] * g;
            }
        }
        Vector color = lit * (1.4f * scaleGlow) + ambient;
        return { std::min(color.x, 1.0f), std::min(color.y, 1.0f), std::min(color.z, 1.0f) };
    }

    bool SceneRenderer::sameLights(const LightState& state) const
    {
        if (state.actors.size() != actorLights.size()) {
            return false;
        }
        for (size_t i = 0; i < actorLights.size(); i++) {
            if (state.actors[i] != actorLights[i] || !sameVector(state.colors[i], actorLightColors[i]) || !samePrepared(state.prepared[i], actorLightPrepared[i])) {
                return false;
            }
        }
        return true;
    }

    void SceneRenderer::storeLights(LightState& state) const
    {
        state.actors = actorLights;
        state.prepared = actorLightPrepared;
        state.colors = actorLightColors;
    }
}

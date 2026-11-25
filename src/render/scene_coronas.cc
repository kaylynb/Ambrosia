#include "render/scene_renderer.h"

#include "engine/props_engine.h"
#include "engine/world.h"
#include "render/lighting.h"
#include "render/scene_renderer_internal.h"

#include <algorithm>

namespace dx {
    void SceneRenderer::addCorona(UObject* light, const Vector& origin, float delta, size_t& free, UObject* viewportActor)
    {
        if (!light || !light->data || light->destroyed || !actor::bCorona(light) || !actor::Skin(light) || actor::bDeleteMe(light)) {
            return;
        }
        World* world = sceneWorld;
        if (!world) {
            return;
        }
        CheckResult hit;
        if (world->lineCheck(hit, origin, actor::Location(light), {}, nullptr, TRACE_Level | TRACE_Movers)) {
            return;
        }
        if (world->lineCheck(hit, origin, actor::Location(light), {}, nullptr, TRACE_Pawns | TRACE_Others) && hit.actor != viewportActor) {
            return;
        }
        for (CoronaLight& corona : coronas) {
            if (corona.actor == light) {
                corona.bright = std::min(corona.bright + 2.0f * delta, 1.0f);
                return;
            }
        }
        while (free < std::size(coronas) && coronas[free].actor) {
            free++;
        }
        if (free < std::size(coronas)) {
            coronas[free].actor = light;
            coronas[free].bright = std::min(2.0f * delta, 1.0f);
        }
    }

    void SceneRenderer::drawCoronas(const View& view, int x, int y, int width, int height)
    {
        UModel* model = levelData.model;
        World* world = sceneWorld;
        UObject* viewportActor = world ? world->player() : nullptr;
        if (!model || !viewportActor || viewportActor->destroyed || !*reinterpret_cast<UObject**>(actor::Region(viewportActor))) {
            return;
        }
        int32_t leaf = *reinterpret_cast<int32_t*>(actor::Region(viewportActor) + sizeof(UObject*));
        if (leaf < 0 || static_cast<size_t>(leaf) >= model->leaves.size()) {
            return;
        }
        if (coronaTime < 0) {
            coronaTime = currentTime;
        }
        float delta = static_cast<float>(3.0 * (currentTime - coronaTime));
        coronaTime = currentTime;
        for (CoronaLight& corona : coronas) {
            if (corona.actor) {
                corona.bright -= delta;
                if (corona.bright < 0) {
                    corona.actor = nullptr;
                }
            }
        }
        size_t free = 0;
        int32_t permeating = model->leaves[static_cast<size_t>(leaf)].permeating;
        if (permeating >= 0) {
            for (size_t i = static_cast<size_t>(permeating); i < model->lights.size() && model->lights[i]; i++) {
                addCorona(model->lights[i], view.location, delta, free, viewportActor);
            }
        }
        for (UObject* light : lights) {
            if ((actor::bStatic(light) || actor::bNoDelete(light)) && !actor::bDynamicLight(light)) {
                continue;
            }
            float maxRadius = std::max(lightRadius(light), 25.0f * (static_cast<float>(actor::VolumeRadius(light)) + 1.0f));
            if (actor::LightRadius(light) == 0 || sphereOutsideView(view, actor::Location(light), maxRadius) || !sphereReachesLeaf(actor::Location(light), lightRadius(light), leaf)) {
                continue;
            }
            addCorona(light, view.location, delta, free, viewportActor);
        }
        UObject* sprite = viewportActor->isA(namePlayerPawn) ? actor::Sprite(viewportActor) : nullptr;
        float halfWidth = static_cast<float>(width) * 0.5f;
        float halfHeight = static_cast<float>(height) * 0.5f;
        float projection = halfWidth / view.tanHalfWidth;
        for (const CoronaLight& corona : coronas) {
            UObject* light = corona.actor;
            if (!light || light->destroyed) {
                continue;
            }
            Vector delta3 = actor::Location(light) - view.location;
            float depth = delta3.dot(view.axes.xAxis);
            if (depth <= 1.0f) {
                continue;
            }
            uint8_t hue = actor::LightHue(light);
            float h = static_cast<float>(hue);
            Vector base;
            if (hue < 86) {
                base = { (85.0f - h) / 85.0f, h / 85.0f, 0.0f };
            } else if (hue < 171) {
                base = { 0.0f, (170.0f - h) / 85.0f, (h - 85.0f) / 85.0f };
            } else {
                base = { (h - 170.0f) / 85.0f, 0.0f, (255.0f - h) / 84.0f };
            }
            float alpha = static_cast<float>(actor::LightSaturation(light)) / 255.0f;
            Vector color = (base + (Vector { 1, 1, 1 } - base) * alpha) * corona.bright;
            float scale = static_cast<float>(width) * actor::DrawScale(light) * 0.2f;
            float screenX = delta3.dot(view.axes.yAxis) * projection / depth + halfWidth + static_cast<float>(x);
            float screenY = -delta3.dot(view.axes.zAxis) * projection / depth + halfHeight + static_cast<float>(y);
            if (UTexture* spriteTexture = asTexture(sprite)) {
                UTexture* frame = device->animated(spriteTexture);
                actor::Skin(light) = frame ? frame : spriteTexture;
            }
            UTexture* skin = asTexture(actor::Skin(light));
            if (!skin) {
                continue;
            }
            device->canvas().drawTile(skin, screenX - scale * 0.5f, screenY - scale * 0.5f, scale, scale, 0, 0, static_cast<float>(skin->width()), static_cast<float>(skin->height()), { color.x, color.y, color.z, 1 }, BlendStyle::Translucent, true);
        }
    }
}

#include "render/scene_renderer.h"

#include "core/scoped_value.h"
#include "engine/mesh.h"
#include "engine/props_engine.h"
#include "engine/world.h"
#include "render/gl_device.h"
#include "render/gpu_profile.h"
#include "render/gpu_timers.h"
#include "render/lighting.h"
#include "render/render_debug.h"
#include "render/scene_renderer_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace dx {
    SceneRenderer::SceneRenderer()
        : device(std::make_unique<GLRenderDevice>())
        , lightmaps(*device)
    {
        startTime = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) / 1e6;
        // The renderer and its texture cache are Holders that drop every cache entry of an
        // object being collected; the library defines no object classes of its own.
        gc::libraryReported("render");
    }

    SceneRenderer::~SceneRenderer()
    {
        flushLevel();
    }

    void SceneRenderer::setTime(double seconds)
    {
        time = seconds;
        timeSet = true;
    }

    void SceneRenderer::setViewportTime(double seconds)
    {
        viewportTime = seconds;
    }

    void SceneRenderer::setPaused(bool paused)
    {
        levelPaused = paused;
    }

    void SceneRenderer::setRenderMode(RenderMode mode)
    {
        renderMode = mode;
    }

    void SceneRenderer::flushLevel()
    {
        lights.clear();
        lightPrepared.clear();
        lightColors.clear();
        lightIndex.clear();
        levelData = LevelData {};
        touchedBatches.clear();
        device->flushCanvas();
        device->clearTextures();
        lightmaps.clear();
        triangleCache.clear();
        poseCaches.clear();
        meshDrawCaches.clear();
        brushDrawCaches.clear();
        actorLightCache.clear();
        for (CoronaLight& corona : coronas) {
            corona = CoronaLight {};
        }
        coronaTime = -1;
        occlusion.clear();

        occlusionIssued.clear();
        occlusionUnconfirmed.clear();
        dropRetired();
        levelReleased = false;
    }

    SceneRenderer::CacheStats SceneRenderer::cacheStats() const
    {
        CacheStats stats;
        auto dead = [](const UObject* actor) {
            return actor && actor->destroyed;
        };
        for (const auto& [key, cache] : meshDrawCaches) {
            stats.meshDraw++;
            stats.meshDrawDead += dead(std::get<0>(key)) ? 1u : 0u;
            stats.meshDrawBytes += sizeof(cache) + cache.world.capacity() * sizeof(Vector) + cache.worldNormals.capacity() * sizeof(Vector) + cache.triangles.capacity() * sizeof(MeshTriangle) + cache.slotInfo.capacity() * sizeof(const GLTextureInfo*) + cache.slotFlags.capacity() * sizeof(uint32_t) + cache.groups.capacity() * sizeof(EmittedGroup);
        }
        for (const auto& [key, caches] : poseCaches) {
            stats.pose++;
            stats.poseDead += dead(key.first) ? 1u : 0u;
            stats.poseBytes += sizeof(caches) + caches.main.verts.capacity() * sizeof(Vector);
            for (const MeshPoseCache& blend : caches.blends) {
                stats.poseBytes += blend.verts.capacity() * sizeof(Vector);
            }
        }
        for (const auto& [actor, senders] : actorLightCache) {
            stats.light++;
            stats.lightDead += dead(actor) ? 1u : 0u;
            stats.lightBytes += sizeof(senders) + senders.capacity() * sizeof(LightSender);
        }
        for (const auto& [actor, state] : occlusion) {
            stats.occlusion++;
            stats.occlusionDead += dead(actor) ? 1u : 0u;
            stats.occlusionQueries += state.query ? 1u : 0u;
        }
        for (const auto& [actor, cache] : brushDrawCaches) {
            stats.brushDraw++;
            stats.brushDrawDead += dead(actor) ? 1u : 0u;
        }
        return stats;
    }

    Canvas2D& SceneRenderer::canvas()
    {
        return device->canvas();
    }

    void SceneRenderer::beginFrame(int width, int height)
    {
        DX_ZONE_NAMED("Begin frame");
        lastRenderCounters = renderCounters;
        renderCounters = {};
        settleReleased();
        if (!timeSet) {
            double now = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) / 1e6;
            time = now - startTime;
        }
        currentTime = viewportTime >= 0 ? viewportTime : time;
        lightDelta = lastFrameTime < 0 ? 0 : std::clamp(static_cast<int32_t>((currentTime - lastFrameTime) * 768.0), 0, 255);
        lastFrameTime = currentTime;
        lightFrameCount++;
        tickLightRandoms(static_cast<float>(time), levelPaused);
        frameWidth = std::max(width, 1);
        frameHeight = std::max(height, 1);
        viewAspect = static_cast<float>(frameWidth) / static_cast<float>(frameHeight);
        device->beginTextureFrame(currentTime);
        offscreen = renderDebug.gamma && device->drawingToWindow() && std::abs(gammaExponent - 1.0f) >= 0.0001f;
        if (offscreen) {
            device->bindGammaTarget(frameWidth, frameHeight);
        }
        device->setViewport(0, 0, frameWidth, frameHeight);
        device->setDepthWrite(true);
        device->clear(true, true, 1.0f);
        device->beginCanvas(frameWidth, frameHeight);
    }

    void SceneRenderer::endFrame()
    {
        device->flushCanvas();
        device->setBlend(BlendMode::Opaque);
    }

    void SceneRenderer::applyBrightness(float brightness)
    {
        float exponent = 1.0f / (2.5f * std::max(brightness, 0.01f));
        gammaExponent = exponent;
        device->flushCanvas();
        if (offscreen) {
            offscreen = false;
            device->resolveGamma(frameWidth, frameHeight, exponent);
            return;
        }
        if (!renderDebug.gamma || std::abs(exponent - 1.0f) < 0.0001f) {
            // Every other brightness keeps the frame in the gamma texture as it goes; here it
            // takes a copy, so a snapshot can always read what was last shown.
            if (device->drawingToWindow()) {
                device->keepFrame(frameWidth, frameHeight);
            }
            return;
        }
        device->applyGamma(frameWidth, frameHeight, exponent);
    }

    bool SceneRenderer::readKeptFrame(std::vector<uint8_t>& rgba, int& width, int& height)
    {
        return device->readKeptFrame(rgba, width, height);
    }

    void SceneRenderer::setOptions(const RenderOptions& newOptions)
    {
        device->setTextureOptions(newOptions.anisotropy, newOptions.trilinear, newOptions.textureRate);
        if (newOptions.smoothLightmaps != options.smoothLightmaps || newOptions.detailDistance != options.detailDistance) {
            device->setQuality(newOptions.smoothLightmaps, newOptions.detailDistance);
        }
        options = newOptions;
    }

    void SceneRenderer::readPixels(std::vector<uint8_t>& rgba, int& width, int& height)
    {
        device->flushCanvas();
        width = frameWidth;
        height = frameHeight;
        rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        std::vector<uint8_t> raw(rgba.size());
        device->readPixels(width, height, raw.data());
        size_t stride = static_cast<size_t>(width) * 4;
        for (size_t y = 0; y < static_cast<size_t>(height); y++) {
            std::copy_n(raw.data() + y * stride, stride, rgba.data() + (static_cast<size_t>(height) - 1 - y) * stride);
        }
    }

    void SceneRenderer::drawScene(World* world, ULevel* level, const std::vector<UObject*>& actors, const Vector& location, const Rotator& rotation, float fov, UObject* viewActor)
    {
        ScopedValue<World*> scope(sceneWorld, world);
        DX_GPU_ZONE("Scene");
        gputimers::Scope sceneGpu(gputimers::Stage::Scene);
        device->flushCanvas();
        bool post = renderDebug.postChain;
        int samples = post ? options.multisample : 0;
        OcclusionQuality ambient = post && renderMode == RenderMode::Textured ? options.ambientOcclusion : OcclusionQuality::Off;
        device->beginScene(frameWidth, frameHeight, samples, ambient);
        drawSceneRect(world, level, actors, location, rotation, fov, viewActor, 0, 0, frameWidth, frameHeight);
        {
            DX_ZONE_NAMED("Post chain");
            DX_GPU_ZONE("Post chain");
            gputimers::Scope postGpu(gputimers::Stage::PostChain);
            device->endScene(frameWidth, frameHeight, post && options.fxaa);
        }
        device->useShader();
    }

    void SceneRenderer::drawSceneRect(World* world, ULevel* level, const std::vector<UObject*>& actors, const Vector& location, const Rotator& rotation, float fov, UObject* viewActor, int x, int y, int width, int height)
    {
        ScopedValue<World*> scope(sceneWorld, world);
        device->flushCanvas();
        settleReleased();
        ScopedValue<int> drawingScope(drawing, drawing + 1);
        if (!level || width <= 0 || height <= 0) {
            return;
        }
        if (levelData.level != level) {
            DX_ZONE_NAMED("Build level render data");
            buildLevel(level, actors);
        }
        bool fullFrame = x == 0 && y == 0 && width == frameWidth && height == frameHeight;
        sceneFrame++;
        {
            DX_ZONE_NAMED("Collect lights");
            collectLights(actors);
            collectDynamicLights(location);
        }
        int bottom = frameHeight - y - height;
        device->setViewport(x, bottom, width, height);
        viewAspect = static_cast<float>(width) / static_cast<float>(height);
        if (!fullFrame) {
            device->enableScissor(x, bottom, width, height);
            device->setDepthWrite(true);
            device->clear(true, true, 1.0f);
        }

        viewportWidth = width;
        viewportHeight = height;
        View view = makeView(location, rotation, fov, sceneNearPlane);
        bool wireframe = renderMode == RenderMode::Wireframe;
        view.zoneMask = wireframe ? ~0ull : visibleZones(pointZone(location));
        if (levelData.model && levelData.levelInfo) {
            for (size_t zone = 0; zone < levelData.model->zones.size(); zone++) {
                if (view.zoneMask & (1ull << (zone & 63))) {
                    levelData.model->zones[zone].lastRenderTime = levelinfo::TimeSeconds(levelData.levelInfo);
                }
            }
        }
        view.fog = true;
        view.sky = renderDebug.sky && levelData.hasBackdrop && frameSkyZone(zoneActor(pointZone(location))) != nullptr;
        gatherMoverMirrors(wireframe ? std::vector<UObject*> {} : actors);
        if (!wireframe) {
            DX_ZONE_NAMED("Visibility");
            occludeView(view, nullptr);
        }
        if (view.sky && !wireframe) {
            DX_ZONE_NAMED("Sky");
            DX_GPU_ZONE("Sky");
            drawSky(view, actors, viewActor);
            device->setDepthWrite(true);
            device->clear(false, true, 1.0f);
        }
        if ((!levelData.mirrors.empty() || !moverMirrors.empty()) && !wireframe && renderDebug.mirrors) {
            DX_ZONE_NAMED("Mirrors");
            DX_GPU_ZONE("Mirrors");
            gputimers::Scope mirrorsGpu(gputimers::Stage::Mirrors);
            drawMirrors(view, actors);
            device->setDepthWrite(true);
            device->clear(false, true, 1.0f);
        }
        {
            DX_ZONE_NAMED("Volumetrics");
            collectVolumetrics(view);
        }
        if (wireframe) {
            drawWireframe(view, actors, viewActor);
        } else {
            device->beginOcclusion();
            drawFrameContent(view, actors, viewActor, fullFrame && renderDebug.occlusionQueries);
            if (renderDebug.coronas) {
                DX_ZONE_NAMED("Coronas");
                drawCoronas(view, x, y, width, height);
            }
        }
        if (renderMode == RenderMode::ZonePortal) {
            drawPortals(view);
        }
        if (fullFrame && !debugBatches.empty()) {
            drawDebugMeshes(view);
        }
        device->setCulling(false);
        device->setDepthTest(false);
        if (!fullFrame) {
            device->disableScissor();
            device->setViewport(0, 0, frameWidth, frameHeight);
        }
        viewAspect = static_cast<float>(frameWidth) / static_cast<float>(frameHeight);
    }

    void SceneRenderer::drawActorOverlay(World* world, UObject* actor, const Vector& viewLocation, const Rotator& viewRotation, float fov, bool clearZ)
    {
        ScopedValue<World*> scope(sceneWorld, world);
        device->flushCanvas();
        settleReleased();
        ScopedValue<int> drawingScope(drawing, drawing + 1);
        if (!actor || !actor->data) {
            return;
        }
        device->setViewport(0, 0, frameWidth, frameHeight);
        if (clearZ) {
            device->setDepthWrite(true);
            device->clear(false, true, 1.0f);
        }
        View view = makeView(viewLocation, viewRotation, fov, clearZ ? overlayNearPlane : sceneNearPlane);
        device->useShader();
        device->setViewProj(view.viewProj);
        device->setPan(0, 0);
        device->setTint(1, 1, 1, 1);
        device->setDepthTest(true);
        device->setDepthFunc(DepthFunc::LessEqual);
        drawActor(actor, view);
        flushMeshBatches();
        flushTranslucent(view);
        device->setCulling(false);
        device->setDepthTest(false);
    }
}

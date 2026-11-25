#include "render/scene_renderer.h"

#include "engine/props_engine.h"
#include "render/gpu_profile.h"
#include "render/gpu_timers.h"
#include "render/render_debug.h"
#include "render/scene_renderer_internal.h"

#include <functional>

namespace dx {
    UObject* frameSkyZone(UObject* zone)
    {
        if (!zone || !zone->data || !zone->isA(nameZoneInfo)) {
            return nullptr;
        }
        UObject* sky = zoneinfo::SkyZone(zone);
        if (!sky || !sky->data || sky->destroyed || !sky->isA(nameSkyZoneInfo)) {
            return nullptr;
        }
        return sky;
    }

    void SceneRenderer::applyClip(const View& view)
    {
        device->setClipPlanes(&view.clipPlanes[0].x, view.clipCount);
    }

    void SceneRenderer::drawFrameContent(const View& view, const std::vector<UObject*>& actors, UObject* viewActor, bool occlude)
    {
        device->useShader();
        applyClip(view);
        drawRecursion = view.recursion;
        drawMoverMirrorDepth(view);
        {
            DX_ZONE_NAMED("Level geometry");
            DX_GPU_ZONE("Level geometry");
            drawLevelGeometry(view, false);
        }
        {
            DX_ZONE_NAMED("Actors");
            DX_GPU_ZONE("Actors");
            drawActors(actors, view, viewActor, occlude);
        }
        if (view.recursion == 0) {
            DX_ZONE_NAMED("Ambient occlusion");
            DX_GPU_ZONE("Ambient occlusion");
            device->applyOcclusion(sceneNearPlane, sceneFarPlane, view.tanHalfWidth, view.tanHalfHeight);
        }
        if (renderDebug.translucent) {
            DX_ZONE_NAMED("Translucent");
            DX_GPU_ZONE("Translucent");
            gputimers::Scope translucentGpu(gputimers::Stage::Translucent);
            drawLevelGeometry(view, true);
        } else {
            translucentCount = 0;
        }
        if (view.clipCount) {
            View plain;
            applyClip(plain);
        }
    }

    void SceneRenderer::drawSky(const View& frame, const std::vector<UObject*>& actors, UObject* viewActor)
    {
        UObject* skyZone = frameSkyZone(zoneActor(pointZone(frame.location)));
        if (!skyZone) {
            return;
        }
        if (!frame.visibility || frame.visibility->backdropSpans.empty()) {
            return;
        }
        Coords skyAxes = rotationAxes(actor::Rotation(skyZone));
        Coords axes;
        axes.xAxis = untransformVector(skyAxes, frame.axes.xAxis);
        axes.yAxis = untransformVector(skyAxes, frame.axes.yAxis);
        axes.zAxis = untransformVector(skyAxes, frame.axes.zAxis);
        View sky = makeView(actor::Location(skyZone), axes, frame.fov, sceneNearPlane);
        uint8_t skyIndex = pointZone(actor::Location(skyZone));
        sky.zoneMask = skyIndex != 0 ? (1ull << (skyIndex & 63)) : ~0ull;
        sky.mirrored = frame.mirrored;
        sky.recursion = frame.recursion + 1;
        sky.sky = true;
        {
            DX_ZONE_NAMED("Sky visibility");
            occludeView(sky, &frame.visibility->backdropSpans);
        }
        device->useShader();
        View plain;
        applyClip(plain);
        drawLevelGeometry(sky, false);
        drawActors(actors, sky, viewActor, false);
        drawLevelGeometry(sky, true);
    }

    void SceneRenderer::markMirror(const View& view, const int32_t* pieces, size_t count)
    {
        device->useShader();
        device->setViewProj(view.viewProj);
        applyClip(view);
        device->setBlend(BlendMode::Opaque);
        device->setFlags(0);
        device->setDepthWrite(true);
        device->clear(false, true, 0.0f);
        device->setDepthTest(true);
        device->setDepthFunc(DepthFunc::Always);
        device->setDepthRange(1.0f, 1.0f);
        device->setCulling(false);
        device->setColorWrite(false);
        for (size_t i = 0; i < count; i++) {
            const MirrorSurface& mirror = levelData.mirrors[static_cast<size_t>(pieces[i])];
            device->drawStatic(levelData.mirrorGeometry.id(), mirror.first, mirror.count);
        }
        device->setColorWrite(true);
        device->setDepthRange(0.0f, 1.0f);
        device->setDepthFunc(DepthFunc::LessEqual);
    }

    void SceneRenderer::gatherMoverMirrors(const std::vector<UObject*>& actors)
    {
        moverMirrors.clear();
        if (!renderDebug.mirrors) {
            return;
        }
        for (UObject* actor : actors) {
            if (!actor || actor->destroyed || !actor->data || actor::DrawType(actor) != DT_Brush || actor::bHidden(actor)) {
                continue;
            }
            UModel* brush = cast<UModel>(actor::Brush(actor));
            if (!brush || !brush->polys) {
                continue;
            }
            Coords rotation = rotationAxes(actor::Rotation(actor));
            Vector scale = scaleOf(brush::MainScale(actor));
            Vector location = actor::Location(actor);
            Vector prePivot = actor::PrePivot(actor);
            for (const Poly& poly : brush->polys->polys) {
                if (poly.vertices.size() < 3 || !(poly.polyFlags & PF_Mirrored)) {
                    continue;
                }
                uint32_t flags = poly.polyFlags | textureFlags(asTexture(poly.texture));
                if (flags & (PF_Invisible | PF_Portal)) {
                    continue;
                }
                MoverMirror& mirror = moverMirrors.emplace_back();
                mirror.actor = actor;
                mirror.polyFlags = flags;
                mirror.normal = untransformVector(rotation, poly.normal * Vector { 1.0f / scale.x, 1.0f / scale.y, 1.0f / scale.z }).normal();
                Vector centre;
                for (const Vector& point : poly.vertices) {
                    mirror.points.push_back(location + untransformVector(rotation, (point - prePivot) * scale));
                    centre += mirror.points.back();
                }
                centre = centre / static_cast<float>(mirror.points.size());
                mirror.distance = mirror.normal.dot(centre);
                mirror.zone = pointZone(centre + mirror.normal * 2.0f);
                for (size_t i = 1; i + 1 < mirror.points.size(); i++) {
                    for (const Vector* point : { &mirror.points[0], &mirror.points[i], &mirror.points[i + 1] }) {
                        GLVertex vertex;
                        vertex.x = point->x;
                        vertex.y = point->y;
                        vertex.z = point->z;
                        mirror.triangles.push_back(vertex);
                    }
                }
            }
        }
    }

    bool SceneRenderer::moverMirrorsActive(int32_t recursion) const
    {
        return renderDebug.mirrors && recursion < MaxFrameRecursion - 1;
    }

    void SceneRenderer::drawMoverMirrorDepth(const View& view)
    {
        if (moverMirrors.empty() || !moverMirrorsActive(view.recursion)) {
            return;
        }
        device->useShader();
        device->setViewProj(view.viewProj);
        device->setDepthTest(true);
        device->setDepthFunc(DepthFunc::LessEqual);
        device->setDepthWrite(true);
        device->setFrontFace(!view.mirrored);
        device->setBlend(BlendMode::Opaque);
        device->setFlags(0);
        device->setColorWrite(false);
        // Pushed back as the mover's own faces are (EmittedGroup::brush), so a level surface on
        // the same plane still wins.
        device->setPolygonOffset(true, 1.0f, 4.0f);
        for (const MoverMirror& mirror : moverMirrors) {
            device->setCulling(!(mirror.polyFlags & PF_TwoSided));
            device->draw(mirror.triangles, Primitive::Triangles);
        }
        device->setPolygonOffset(false, 0.0f, 0.0f);
        device->setColorWrite(true);
    }

    void SceneRenderer::markMoverMirror(const View& view, const MoverMirror& mirror)
    {
        device->useShader();
        device->setViewProj(view.viewProj);
        applyClip(view);
        device->setBlend(BlendMode::Opaque);
        device->setFlags(0);
        device->setDepthWrite(true);
        device->clear(false, true, 0.0f);
        device->setDepthTest(true);
        device->setDepthFunc(DepthFunc::Always);
        device->setDepthRange(1.0f, 1.0f);
        device->setCulling(false);
        device->setColorWrite(false);
        device->draw(mirror.triangles, Primitive::Triangles);
        device->setColorWrite(true);
        device->setDepthRange(0.0f, 1.0f);
        device->setDepthFunc(DepthFunc::LessEqual);
    }

    void SceneRenderer::drawMirrors(const View& view, const std::vector<UObject*>& actors)
    {
        if (view.recursion >= MaxFrameRecursion - 1) {
            return;
        }
        // One mirror view through a mirror plane: a level group of surfaces or a mover face.
        // pieceHulls, for a group of several pieces, are each piece's hull with whether it shows.
        auto reflectThrough = [&](Vector mirrorNormal, float mirrorDistance, uint8_t zone, uint32_t polyFlags, const std::vector<Vector>& hull, const std::vector<ScreenSpan>& open, const std::vector<const std::vector<Vector>*>& pieceHulls, const std::function<void()>& mark) {
            if (open.empty() || !(view.zoneMask & (1ull << (zone & 63)))) {
                return;
            }
            float side = mirrorNormal.dot(view.location) - mirrorDistance;
            if (side < -1.0f && !(polyFlags & PF_TwoSided)) {
                return;
            }
            Vector normal = side >= 0 ? mirrorNormal : -mirrorNormal;
            float distance = side >= 0 ? mirrorDistance : -mirrorDistance;
            auto reflect = [&](const Vector& direction) {
                return direction - normal * (2.0f * normal.dot(direction));
            };
            auto reflectPoint = [&](const Vector& point) {
                return point - normal * (2.0f * (normal.dot(point) - distance));
            };
            Coords axes;
            axes.xAxis = reflect(view.axes.xAxis);
            axes.yAxis = reflect(view.axes.yAxis);
            axes.zAxis = reflect(view.axes.zAxis);
            View child = makeView(reflectPoint(view.location), axes, view.fov, sceneNearPlane);
            child.zoneMask = visibleZones(zone);
            child.recursion = view.recursion + 1;
            child.mirrored = !view.mirrored;
            child.fog = view.fog;
            child.sky = renderDebug.sky && levelData.hasBackdrop && frameSkyZone(zoneActor(pointZone(child.location))) != nullptr;
            child.clipPlanes[0] = { normal.x, normal.y, normal.z, -distance };
            child.clipCount = 1;
            auto pyramid = [&](const std::vector<Vector>& polygon) {
                std::vector<Plane> planes;
                if (polygon.size() < 3) {
                    return planes;
                }
                Vector middle;
                for (const Vector& point : polygon) {
                    middle += point;
                }
                middle = middle / static_cast<float>(polygon.size());
                for (size_t i = 0; i < polygon.size(); i++) {
                    Vector a = polygon[i] - child.location;
                    Vector b = polygon[(i + 1) % polygon.size()] - child.location;
                    Vector edgeNormal = a.cross(b).normal();
                    if (edgeNormal.dot(middle - child.location) < 0) {
                        edgeNormal = -edgeNormal;
                    }
                    planes.push_back({ edgeNormal.x, edgeNormal.y, edgeNormal.z, -edgeNormal.dot(child.location) });
                }
                return planes;
            };
            if (!view.portal.empty()) {
                std::vector<Vector> inherited;
                for (const Vector& point : view.portal) {
                    inherited.push_back(reflectPoint(point));
                }
                std::vector<Plane> planes = pyramid(inherited);
                if (child.clipCount + static_cast<int32_t>(planes.size()) <= 8) {
                    for (const Plane& plane : planes) {
                        child.clipPlanes[static_cast<size_t>(child.clipCount++)] = plane;
                    }
                }
            }
            child.cullPlanes = pyramid(hull);
            // An actor must also lie in the pyramid of one piece that shows, as it had to when
            // each piece was a view of its own: the hull of the pieces also covers the floor
            // between them, and an actor drawn there sets LastRenderTime, which scripts read.
            for (const std::vector<Vector>* pieceHull : pieceHulls) {
                child.cullPieces.push_back(pyramid(*pieceHull));
            }
            child.cullPlanes.push_back(child.clipPlanes[0]);
            child.portal = hull;
            {
                DX_ZONE_NAMED("Mirror visibility");
                occludeView(child, &open);
            }
            {
                DX_ZONE_NAMED("Nested mirrors");
                drawMirrors(child, actors);
            }
            if (child.sky) {
                DX_ZONE_NAMED("Mirror sky");
                mark();
                drawSky(child, actors, nullptr);
            }
            {
                DX_ZONE_NAMED("Mirror mark");
                mark();
            }
            if (child.fog) {
                DX_ZONE_NAMED("Mirror volumetrics");
                collectVolumetrics(child);
            }
            drawFrameContent(child, actors, nullptr, false);
        };
        // One mirror view per group of mirror surfaces on one plane in one zone: the reflection is
        // the same through each of them, so they share one visibility pass, sky view and actor
        // pass instead of repeating them per surface.
        std::vector<const std::vector<Vector>*> pieceHulls;
        for (const MirrorPlane& group : levelData.mirrorPlanes) {
            if (!levelData.mirrorGeometry) {
                break;
            }
            const int32_t* pieces = group.pieces.data();
            size_t pieceCount = group.pieces.size();
            const MirrorSurface& mirror = levelData.mirrors[static_cast<size_t>(pieces[0])];
            const std::vector<ScreenSpan>* openSpans = &view.visibility->mirrorSpans[static_cast<size_t>(pieces[0])];
            pieceHulls.clear();
            if (pieceCount > 1) {
                std::vector<ScreenSpan>& joined = scratchMirrorSpans;
                joined.clear();
                for (size_t i = 0; i < pieceCount; i++) {
                    const std::vector<ScreenSpan>& piece = view.visibility->mirrorSpans[static_cast<size_t>(pieces[i])];
                    joined.insert(joined.end(), piece.begin(), piece.end());
                    if (!piece.empty()) {
                        pieceHulls.push_back(&levelData.mirrors[static_cast<size_t>(pieces[i])].hull);
                    }
                }
                openSpans = &joined;
            }
            reflectThrough(mirror.normal, mirror.distance, mirror.zone, mirror.polyFlags, group.hull, *openSpans, pieceHulls, [&]() {
                markMirror(view, pieces, pieceCount);
            });
        }
        // Mirror faces on movers, each a view of its own.
        if (moverMirrorsActive(view.recursion)) {
            pieceHulls.clear();
            for (size_t index = 0; index < moverMirrors.size() && index < view.visibility->moverMirrorSpans.size(); index++) {
                const MoverMirror& mirror = moverMirrors[index];
                reflectThrough(mirror.normal, mirror.distance, mirror.zone, mirror.polyFlags, mirror.points, view.visibility->moverMirrorSpans[index], pieceHulls, [&]() {
                    markMoverMirror(view, mirror);
                });
            }
        }
    }

    void SceneRenderer::applyView(const View& view)
    {
        device->useShader();
        device->setViewProj(view.viewProj);
        device->setPan(0, 0);
        device->setTint(1, 1, 1, 1);
        device->setDepthTest(true);
        device->setDepthFunc(DepthFunc::LessEqual);
        device->setFrontFace(!view.mirrored);
    }

    uint32_t SceneRenderer::flatVao()
    {
        size_t index = renderMode == RenderMode::ZonePortal ? 0 : 1;
        if (levelData.flatGeometry[index] || levelData.vertices.empty()) {
            return levelData.flatGeometry[index].id();
        }
        std::vector<GLVertex> vertices = levelData.vertices;
        for (const Batch& batch : levelData.batches) {
            Vector textureColor { 0.5f, 0.5f, 0.5f };
            if (batch.texture && batch.texture->data) {
                Color mip = bitmap::MipZero(batch.texture);
                if (mip.r || mip.g || mip.b) {
                    textureColor = Vector { static_cast<float>(mip.r), static_cast<float>(mip.g), static_cast<float>(mip.b) } / 255.0f;
                }
            }
            for (const auto& [first, count, node] : batch.nodePolys) {
                Vector color = index == 0 ? zoneColor(batch.zone, node, textureColor) : bspCutColor(node);
                for (int32_t i = first; i < first + count; i++) {
                    vertices[static_cast<size_t>(i)].r = color.x;
                    vertices[static_cast<size_t>(i)].g = color.y;
                    vertices[static_cast<size_t>(i)].b = color.z;
                }
            }
        }
        levelData.flatGeometry[index] = device->createStatic(vertices);
        return levelData.flatGeometry[index].id();
    }

    void SceneRenderer::drawWireframe(const View& view, const std::vector<UObject*>& actors, UObject* viewActor)
    {
        device->useShader();
        applyClip(view);
        applyView(view);
        device->setScale(1, 1);
        device->setTint(0.6f, 0.6f, 0.6f, 1);
        device->setBlend(BlendMode::Opaque);
        device->setFlags(0);
        device->setDepthTest(false);
        device->draw(levelData.wireVertices, Primitive::Lines);
        device->setTint(1, 1, 1, 1);
        device->setWireframe(true);
        drawActors(actors, view, viewActor, false);
        flushTranslucent(view);
        device->setWireframe(false);
    }

    void SceneRenderer::setDebugMeshes(int layer, std::vector<DebugMesh> meshes)
    {
        std::vector<DebugBatch>& batches = debugBatches[layer];
        batches.clear();
        auto vertices = [](const std::vector<Vector>& points) {
            std::vector<GLVertex> out(points.size());
            for (size_t i = 0; i < points.size(); i++) {
                out[i].x = points[i].x;
                out[i].y = points[i].y;
                out[i].z = points[i].z;
            }
            return out;
        };
        for (DebugMesh& mesh : meshes) {
            DebugBatch batch;
            batch.triangles = vertices(mesh.triangles);
            batch.lines = vertices(mesh.lines);
            mesh.triangles.clear();
            mesh.lines.clear();
            batch.mesh = std::move(mesh);
            batches.push_back(std::move(batch));
        }
        if (batches.empty()) {
            debugBatches.erase(layer);
        }
    }

    void SceneRenderer::drawDebugMeshes(const View& view)
    {
        applyView(view);
        device->setScale(1, 1);
        device->setBlend(BlendMode::Alpha);
        device->setFlags(0);
        device->setCulling(false);
        device->setDepthWrite(false);
        // Pulled towards the eye so a shaded surface wins against the wall it lies on.
        device->setPolygonOffset(true, -1.0f, -4.0f);
        for (const auto& [layer, batches] : debugBatches) {
            for (const DebugBatch& batch : batches) {
                device->setDepthTest(!batch.mesh.throughWalls);
                device->setTint(batch.mesh.r, batch.mesh.g, batch.mesh.b, batch.mesh.a);
                if (!batch.triangles.empty()) {
                    device->draw(batch.triangles, Primitive::Triangles);
                }
                if (!batch.lines.empty()) {
                    device->setTint(batch.mesh.r, batch.mesh.g, batch.mesh.b, 1.0f);
                    device->draw(batch.lines, Primitive::Lines);
                }
            }
        }
        device->setPolygonOffset(false, 0, 0);
        device->setDepthTest(true);
        device->setDepthWrite(true);
        device->setTint(1, 1, 1, 1);
        device->setBlend(BlendMode::Opaque);
    }

    void SceneRenderer::drawPortals(const View& view)
    {
        applyView(view);
        device->setScale(1, 1);
        device->setTint(1.0f, 0.25f, 0.9f, 0.45f);
        device->setBlend(BlendMode::Alpha);
        device->setFlags(0);
        device->setCulling(false);
        device->draw(levelData.portalVertices, Primitive::Triangles);
        device->setTint(1, 1, 1, 1);
        device->setBlend(BlendMode::Opaque);
    }
}

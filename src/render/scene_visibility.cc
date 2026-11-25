#include "render/scene_renderer.h"

#include "core/objects.h"
#include "core/profile.h"
#include "engine/mesh.h"
#include "engine/props_engine.h"
#include "render/scene_renderer_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dx {
    SceneRenderer::View SceneRenderer::makeView(const Vector& location, const Rotator& rotation, float fov, float nearPlane)
    {
        View view = makeView(location, rotationAxes(rotation), fov, nearPlane);
        view.rotation = rotation;
        return view;
    }

    SceneRenderer::View SceneRenderer::makeView(const Vector& location, const Coords& axes, float fov, float nearPlane)
    {
        View view;
        view.location = location;
        view.fov = fov;
        view.axes = axes;
        float aspect = viewAspect;
        view.tanHalfWidth = std::tan(fov * Pi / 360.0f);
        view.tanHalfHeight = view.tanHalfWidth / aspect;
        view.viewProj = mat4Multiply(projectionMatrix(fov, aspect, nearPlane, sceneFarPlane), viewMatrix(location, axes));
        return view;
    }

    UObject* SceneRenderer::zoneActor(uint8_t zone) const
    {
        if (levelData.model && zone < levelData.model->zones.size()) {
            UObject* actor = levelData.model->zones[zone].zoneActor;
            if (actor) {
                objects::ensureLoaded(actor);
                return actor;
            }
        }
        return levelData.levelInfo;
    }

    uint8_t SceneRenderer::pointZone(const Vector& p) const
    {
        UModel* model = levelData.model;
        if (!model || model->nodes.empty()) {
            return 0;
        }
        int32_t node = 0;
        uint8_t zoneIndex = 0;
        for (int guard = 0; guard < 10000 && node >= 0 && static_cast<size_t>(node) < model->nodes.size(); guard++) {
            const BspNode& current = model->nodes[static_cast<size_t>(node)];
            float distance = current.plane.x * p.x + current.plane.y * p.y + current.plane.z * p.z - current.plane.w;
            bool front = distance >= 0;
            zoneIndex = current.zone[front ? 1 : 0];
            int32_t next = front ? current.front : current.back;
            if (next <= 0) {
                break;
            }
            node = next;
        }
        return zoneIndex;
    }

    int32_t SceneRenderer::pointLeaf(const Vector& p) const
    {
        UModel* model = levelData.model;
        if (!model || model->nodes.empty()) {
            return -1;
        }
        int32_t node = 0;
        int32_t leaf = -1;
        for (int guard = 0; guard < 10000 && node >= 0 && static_cast<size_t>(node) < model->nodes.size(); guard++) {
            const BspNode& current = model->nodes[static_cast<size_t>(node)];
            float distance = current.plane.x * p.x + current.plane.y * p.y + current.plane.z * p.z - current.plane.w;
            bool front = distance >= 0;
            leaf = current.leaf[front ? 1 : 0];
            int32_t next = front ? current.front : current.back;
            if (next <= 0) {
                break;
            }
            node = next;
        }
        return leaf;
    }

    uint8_t SceneRenderer::actorZone(UObject* actor) const
    {
        uint8_t* region = actor::Region(actor);
        if (*reinterpret_cast<UObject**>(region)) {
            return region[sizeof(UObject*) + sizeof(int32_t)];
        }
        return pointZone(actor::Location(actor));
    }

    UObject* SceneRenderer::zoneForActor(UObject* actor)
    {
        UObject* zone = *reinterpret_cast<UObject**>(actor::Region(actor));
        if (zone) {
            return zone;
        }
        return zoneActor(pointZone(actor::Location(actor)));
    }

    uint64_t SceneRenderer::visibleZones(uint8_t zone) const
    {
        UModel* model = levelData.model;
        if (!model || zone == 0 || zone >= model->zones.size()) {
            return ~0ull;
        }
        uint64_t mask = model->zones[zone].visibility;
        if (mask == 0) {
            return ~0ull;
        }
        return mask | 1ull | (1ull << zone);
    }

    bool SceneRenderer::inFrustum(const View& view, const Vector& center, float radius) const
    {
        Vector delta = center - view.location;
        float forward = delta.dot(view.axes.xAxis);
        if (forward < -radius) {
            return false;
        }
        if (forward > 0 && radius * static_cast<float>(frameWidth) * 0.5f < forward * view.tanHalfWidth) {
            return false;
        }
        for (const Plane& plane : view.cullPlanes) {
            if (plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w < -radius) {
                return false;
            }
        }
        if (!view.cullPieces.empty()) {
            bool inPiece = false;
            for (const std::vector<Plane>& piece : view.cullPieces) {
                bool inside = true;
                for (const Plane& plane : piece) {
                    if (plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w < -radius) {
                        inside = false;
                        break;
                    }
                }
                if (inside) {
                    inPiece = true;
                    break;
                }
            }
            if (!inPiece) {
                return false;
            }
        }
        return !sphereOutsideView(view, center, radius);
    }

    void SceneRenderer::occludeView(View& view, const std::vector<ScreenSpan>* open)
    {
        DX_TIME("Vis: occludeView");
        std::shared_ptr<ViewVisibility> visibility = std::make_shared<ViewVisibility>();
        view.visibility = visibility;
        visibility->mirrorSpans.resize(levelData.mirrors.size());
        UModel* model = levelData.model;
        if (!model || model->nodes.empty()) {
            return;
        }
        if (open) {
            spans.reset(viewportWidth, viewportHeight, *open);
        } else {
            spans.reset(viewportWidth, viewportHeight);
        }
        const Vector& eye = view.location;
        const Coords& axes = view.axes;
        float halfWidth = static_cast<float>(viewportWidth) * 0.5f;
        float halfHeight = static_cast<float>(viewportHeight) * 0.5f;
        float scaleX = halfWidth / view.tanHalfWidth;
        float scaleY = halfHeight / view.tanHalfHeight;
        float guardWidth = view.tanHalfWidth * 1.02f;
        float guardHeight = view.tanHalfHeight * 1.02f;
        std::array<Plane, 13> planes;
        int32_t planeCount = 0;
        auto addPlane = [&](const Vector& normal, const Vector& point) {
            planes[static_cast<size_t>(planeCount++)] = { normal.x, normal.y, normal.z, -normal.dot(point) };
        };
        addPlane(axes.xAxis, eye + axes.xAxis * sceneNearPlane);
        addPlane(axes.xAxis * guardWidth - axes.yAxis, eye);
        addPlane(axes.xAxis * guardWidth + axes.yAxis, eye);
        addPlane(axes.xAxis * guardHeight - axes.zAxis, eye);
        addPlane(axes.xAxis * guardHeight + axes.zAxis, eye);
        for (int32_t i = 0; i < view.clipCount; i++) {
            planes[static_cast<size_t>(planeCount++)] = view.clipPlanes[static_cast<size_t>(i)];
        }
        auto distance = [](const Plane& plane, const Vector& point) {
            return plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w;
        };
        auto project = [&](const Vector& point) {
            Vector delta = point - eye;
            float forward = delta.dot(axes.xAxis);
            return ScreenPoint { halfWidth + delta.dot(axes.yAxis) / forward * scaleX, halfHeight - delta.dot(axes.zAxis) / forward * scaleY };
        };
        auto boxVisible = [&](const BoundingBox& box) {
            Vector corners[8];
            for (int32_t c = 0; c < 8; c++) {
                corners[c] = { (c & 1) ? box.max.x : box.min.x, (c & 2) ? box.max.y : box.min.y, (c & 4) ? box.max.z : box.min.z };
            }
            for (int32_t p = 0; p < planeCount; p++) {
                bool outside = true;
                for (const Vector& corner : corners) {
                    if (distance(planes[static_cast<size_t>(p)], corner) >= 0) {
                        outside = false;
                        break;
                    }
                }
                if (outside) {
                    return false;
                }
            }
            float left = std::numeric_limits<float>::max();
            float right = std::numeric_limits<float>::lowest();
            float top = std::numeric_limits<float>::max();
            float bottom = std::numeric_limits<float>::lowest();
            for (const Vector& corner : corners) {
                if ((corner - eye).dot(axes.xAxis) < sceneNearPlane) {
                    return spans.rectVisible(0, 0, viewportWidth, viewportHeight);
                }
                ScreenPoint point = project(corner);
                left = std::min(left, point.x);
                right = std::max(right, point.x);
                top = std::min(top, point.y);
                bottom = std::max(bottom, point.y);
            }
            left = std::clamp(left, -1.0f, static_cast<float>(viewportWidth) + 1.0f);
            right = std::clamp(right, -1.0f, static_cast<float>(viewportWidth) + 1.0f);
            top = std::clamp(top, -1.0f, static_cast<float>(viewportHeight) + 1.0f);
            bottom = std::clamp(bottom, -1.0f, static_cast<float>(viewportHeight) + 1.0f);
            return spans.rectVisible(static_cast<int32_t>(std::floor(left)) - 1, static_cast<int32_t>(std::floor(top)) - 1, static_cast<int32_t>(std::ceil(right)) + 1, static_cast<int32_t>(std::ceil(bottom)) + 1);
        };
        bool mirrorsActive = renderDebug.mirrors && view.recursion < MaxFrameRecursion - 1;
        // Mover mirror faces, before any occluder goes in: where they fall on screen, within the
        // spans this view was opened with. What stands in front is drawn over the reflection
        // later, and their depth pass keeps what is behind from covering it.
        visibility->moverMirrorSpans.assign(moverMirrorsActive(view.recursion) ? moverMirrors.size() : 0, {});
        for (size_t index = 0; index < visibility->moverMirrorSpans.size(); index++) {
            const MoverMirror& mirror = moverMirrors[index];
            if (!(view.zoneMask & (1ull << (mirror.zone & 63)))) {
                continue;
            }
            std::vector<Vector> input(mirror.points.begin(), mirror.points.end());
            std::vector<Vector> output;
            for (int32_t p = 0; p < planeCount && input.size() >= 3; p++) {
                const Plane& plane = planes[static_cast<size_t>(p)];
                output.clear();
                for (size_t i = 0; i < input.size(); i++) {
                    const Vector& a = input[i];
                    const Vector& b = input[(i + 1) % input.size()];
                    float da = distance(plane, a);
                    float db = distance(plane, b);
                    if (da >= 0) {
                        output.push_back(a);
                    }
                    if ((da >= 0) != (db >= 0)) {
                        output.push_back(a + (b - a) * (da / (da - db)));
                    }
                }
                std::swap(input, output);
            }
            if (input.size() < 3 || input.size() > 64) {
                continue;
            }
            ScreenPoint projected[64];
            for (size_t i = 0; i < input.size(); i++) {
                projected[i] = project(input[i]);
            }
            spans.polygonVisible(projected, static_cast<int32_t>(input.size()), &visibility->moverMirrorSpans[index]);
        }
        const std::vector<BspNode>& nodes = model->nodes;
        int32_t sequence = 0;
        Vector clipA[64];
        Vector clipB[64];
        ScreenPoint screen[64];
        std::vector<ScreenPoint> pendingPoints;
        std::vector<int32_t> pendingCounts;
        auto processPolygon = [&](int32_t index) {
            const BspNode& node = nodes[static_cast<size_t>(index)];
            if (node.numVertices < 3 || node.surf < 0 || static_cast<size_t>(node.surf) >= levelData.surfFlags.size()) {
                return;
            }
            uint32_t flags = levelData.surfFlags[static_cast<size_t>(node.surf)];
            float side = node.plane.x * eye.x + node.plane.y * eye.y + node.plane.z * eye.z - node.plane.w;
            bool twoSided = (flags & (PF_TwoSided | PF_Portal)) != 0;
            if (side < -1.0f && !twoSided) {
                return;
            }
            bool drawn = false;
            for (int32_t e = levelData.nodeEmitStart[static_cast<size_t>(index)]; e < levelData.nodeEmitStart[static_cast<size_t>(index) + 1]; e++) {
                if (view.zoneMask & (1ull << (levelData.batches[static_cast<size_t>(levelData.nodeEmits[static_cast<size_t>(e)].batch)].zone & 63))) {
                    drawn = true;
                    break;
                }
            }
            if (!drawn) {
                return;
            }
            Vector* input = clipA;
            Vector* output = clipB;
            int32_t count = std::min<int32_t>(node.numVertices, 32);
            for (int32_t i = 0; i < count; i++) {
                input[i] = model->points[static_cast<size_t>(model->verts[static_cast<size_t>(node.vertPool + i)].vertex)];
            }
            for (int32_t p = 0; p < planeCount && count >= 3; p++) {
                const Plane& plane = planes[static_cast<size_t>(p)];
                int32_t produced = 0;
                for (int32_t i = 0; i < count; i++) {
                    const Vector& a = input[i];
                    const Vector& b = input[(i + 1) % count];
                    float da = distance(plane, a);
                    float db = distance(plane, b);
                    if (da >= 0) {
                        output[produced++] = a;
                    }
                    if ((da >= 0) != (db >= 0) && produced < 63) {
                        output[produced++] = a + (b - a) * (da / (da - db));
                    }
                }
                std::swap(input, output);
                count = produced;
            }
            if (count < 3) {
                return;
            }
            for (int32_t i = 0; i < count; i++) {
                screen[i] = project(input[i]);
            }
            int32_t mirror = mirrorsActive && (flags & PF_Mirrored) ? levelData.nodeMirror[static_cast<size_t>(index)] : -1;
            bool backdrop = (flags & PF_FakeBackdrop) && view.sky;
            scratchSpans.clear();
            if (!spans.polygonVisible(screen, count, (mirror >= 0 || backdrop) ? &scratchSpans : nullptr)) {
                return;
            }
            visibility->nodes.push_back(index);
            visibility->nodeSequence.push_back(sequence++);
            if (mirror >= 0) {
                std::vector<ScreenSpan>& target = visibility->mirrorSpans[static_cast<size_t>(mirror)];
                target.insert(target.end(), scratchSpans.begin(), scratchSpans.end());
            } else if (backdrop) {
                visibility->backdropSpans.insert(visibility->backdropSpans.end(), scratchSpans.begin(), scratchSpans.end());
            }
            bool occluder = mirror >= 0 || backdrop || !(flags & (PF_Masked | PF_Translucent | PF_Modulated | PF_Invisible | PF_Portal));
            if (occluder && (side > 0 || twoSided)) {
                pendingCounts.push_back(count);
                pendingPoints.insert(pendingPoints.end(), screen, screen + count);
            }
        };
        auto visitLeaf = [&](int32_t leaf) {
            if (leaf >= 0 && static_cast<size_t>(leaf) < model->leaves.size()) {
                visibility->leaves.emplace_back(leaf, sequence);
            }
            sequence++;
        };
        std::vector<std::pair<int32_t, int32_t>> stack;
        stack.emplace_back(0, 0);
        while (!stack.empty() && !spans.full()) {
            auto [index, stage] = stack.back();
            stack.pop_back();
            const BspNode& node = nodes[static_cast<size_t>(index)];
            // URender::OccludeBsp puts an eye exactly on a plane behind it.
            bool front = node.plane.x * eye.x + node.plane.y * eye.y + node.plane.z * eye.z - node.plane.w > 0;
            if (stage == 0) {
                bool zoneHidden = view.zoneMask != ~0ull && node.zoneMask && !(node.zoneMask & view.zoneMask);
                bool boxHidden = false;
                if (!zoneHidden && node.renderBound >= 0 && static_cast<size_t>(node.renderBound) < model->nodeBounds.size() && model->nodeBounds[static_cast<size_t>(node.renderBound)].valid) {
                    boxHidden = !boxVisible(model->nodeBounds[static_cast<size_t>(node.renderBound)]);
                }
                if (zoneHidden || boxHidden) {
                    visibility->culled.emplace_back(index, sequence++);
                    continue;
                }
                stack.emplace_back(index, 1);
                int32_t nearChild = front ? node.front : node.back;
                if (nearChild > 0) {
                    stack.emplace_back(nearChild, 0);
                } else {
                    visitLeaf(node.leaf[front ? 1 : 0]);
                }
                continue;
            }
            pendingPoints.clear();
            pendingCounts.clear();
            for (int32_t plane = index; plane >= 0 && static_cast<size_t>(plane) < nodes.size();) {
                processPolygon(plane);
                int32_t next = nodes[static_cast<size_t>(plane)].iPlane;
                if (next == plane) {
                    break;
                }
                plane = next;
            }
            size_t offset = 0;
            for (int32_t count : pendingCounts) {
                spans.occlude(pendingPoints.data() + offset, count);
                offset += static_cast<size_t>(count);
            }
            int32_t farChild = front ? node.back : node.front;
            if (farChild > 0) {
                stack.emplace_back(farChild, 0);
            } else {
                visitLeaf(node.leaf[front ? 0 : 1]);
            }
        }
        visibility->endSequence = sequence;
    }

    void SceneRenderer::gatherVisible(const View& view)
    {
        for (int32_t batch : touchedBatches) {
            levelData.batchRanges[static_cast<size_t>(batch)].clear();
        }
        touchedBatches.clear();
        visibleStamp++;
        if (!view.visibility) {
            return;
        }
        UModel* model = levelData.model;
        const ViewVisibility& visibility = *view.visibility;
        for (size_t i = 0; i < visibility.nodes.size(); i++) {
            nodeOrder[static_cast<size_t>(visibility.nodes[i])] = -visibility.nodeSequence[i];
        }
        for (const auto& [leaf, sequence] : visibility.leaves) {
            leafOrder[static_cast<size_t>(leaf)] = -sequence;
            levelData.leafStamp[static_cast<size_t>(leaf)] = visibleStamp;
        }
        for (const auto& [node, sequence] : visibility.culled) {
            levelData.cullOrder[static_cast<size_t>(node)] = -sequence;
            levelData.cullStamp[static_cast<size_t>(node)] = visibleStamp;
        }
        fallbackOrder = -visibility.endSequence - 1;
        for (int32_t node : visibility.nodes) {
            levelData.nodeStamp[static_cast<size_t>(node)] = visibleStamp;
            uint32_t& surfStamp = levelData.surfStamp[static_cast<size_t>(model->nodes[static_cast<size_t>(node)].surf)];
            renderCounters.visibleSurfaces += surfStamp != visibleStamp ? 1u : 0u;
            surfStamp = visibleStamp;
            for (int32_t e = levelData.nodeEmitStart[static_cast<size_t>(node)]; e < levelData.nodeEmitStart[static_cast<size_t>(node) + 1]; e++) {
                const NodeEmit& emit = levelData.nodeEmits[static_cast<size_t>(e)];
                std::vector<std::array<int32_t, 3>>& ranges = levelData.batchRanges[static_cast<size_t>(emit.batch)];
                if (ranges.empty()) {
                    touchedBatches.push_back(emit.batch);
                }
                ranges.push_back({ emit.first, emit.count, node });
            }
        }
    }

    bool SceneRenderer::sphereReachesVisible(const View& view, const Vector& center, float radius)
    {
        UModel* model = levelData.model;
        if (!view.visibility || !model || model->nodes.empty()) {
            return true;
        }
        int32_t leaf = pointLeaf(center);
        if (leaf >= 0 && static_cast<size_t>(leaf) < levelData.leafStamp.size() && levelData.leafStamp[static_cast<size_t>(leaf)] == visibleStamp) {
            return true;
        }
        std::vector<int32_t>& stack = scratchNodes;
        stack.clear();
        stack.push_back(0);
        for (int32_t budget = 256; !stack.empty(); budget--) {
            if (budget <= 0) {
                return true;
            }
            int32_t index = stack.back();
            stack.pop_back();
            if (levelData.cullStamp[static_cast<size_t>(index)] == visibleStamp) {
                continue;
            }
            const BspNode& node = model->nodes[static_cast<size_t>(index)];
            float distance = node.plane.x * center.x + node.plane.y * center.y + node.plane.z * center.z - node.plane.w;
            for (int32_t side = 1; side >= 0; side--) {
                if (side ? distance <= -radius : distance >= radius) {
                    continue;
                }
                int32_t child = side ? node.front : node.back;
                if (child > 0) {
                    stack.push_back(child);
                    continue;
                }
                int32_t touched = node.leaf[side];
                if (touched >= 0 && static_cast<size_t>(touched) < levelData.leafStamp.size() && levelData.leafStamp[static_cast<size_t>(touched)] == visibleStamp) {
                    return true;
                }
            }
        }
        return false;
    }

    float SceneRenderer::actorRadius(UObject* actor) const
    {
        float drawScale = std::max(std::abs(actor::DrawScale(actor)), 0.01f);
        UObject* meshObject = actor::Mesh(actor);
        if (actor::DrawType(actor) == DT_Mesh && meshObject) {
            UMesh* mesh = cast<UMesh>(meshObject);
            if (mesh && mesh->meshBounds.valid) {
                Vector extent = (mesh->meshBounds.max - mesh->meshBounds.min) * 0.5f;
                float scale = std::max({ std::abs(mesh->scale.x), std::abs(mesh->scale.y), std::abs(mesh->scale.z) });
                return extent.size() * scale * drawScale + 32.0f;
            }
            return 256.0f * drawScale;
        }
        if (actor::DrawType(actor) == DT_Brush) {
            UObject* brushObject = actor::Brush(actor);
            if (brushObject) {
                objects::ensureLoaded(brushObject);
            }
            UModel* brush = brushObject ? cast<UModel>(brushObject) : nullptr;
            if (!brush || !brush->bounds.valid) {
                return 4096.0f;
            }
            Vector scale = scaleOf(brush::MainScale(actor));
            Vector prePivot = actor::PrePivot(actor);
            float radius = 0;
            for (int32_t c = 0; c < 8; c++) {
                Vector corner { (c & 1) ? brush->bounds.max.x : brush->bounds.min.x, (c & 2) ? brush->bounds.max.y : brush->bounds.min.y, (c & 4) ? brush->bounds.max.z : brush->bounds.min.z };
                radius = std::max(radius, ((corner - prePivot) * scale).size());
            }
            return radius + 32.0f;
        }
        return 128.0f * drawScale + 32.0f;
    }

    bool SceneRenderer::sphereOutsideView(const View& view, const Vector& location, float radius) const
    {
        Vector delta = location - view.location;
        float forward = delta.dot(view.axes.xAxis);
        float right = delta.dot(view.axes.yAxis);
        float up = delta.dot(view.axes.zAxis);
        float widthScale = 1.0f / std::sqrt(1.0f + view.tanHalfWidth * view.tanHalfWidth);
        float heightScale = 1.0f / std::sqrt(1.0f + view.tanHalfHeight * view.tanHalfHeight);
        return (forward * view.tanHalfWidth - right) * widthScale < -radius || (forward * view.tanHalfWidth + right) * widthScale < -radius || (forward * view.tanHalfHeight - up) * heightScale < -radius || (forward * view.tanHalfHeight + up) * heightScale < -radius;
    }

    bool SceneRenderer::sphereReachesLeaf(const Vector& center, float radius, int32_t leaf) const
    {
        UModel* model = levelData.model;
        if (!model || model->nodes.empty() || leaf < 0) {
            return false;
        }
        std::vector<int32_t> stack { 0 };
        while (!stack.empty()) {
            const BspNode& node = model->nodes[static_cast<size_t>(stack.back())];
            stack.pop_back();
            float distance = node.plane.x * center.x + node.plane.y * center.y + node.plane.z * center.z - node.plane.w;
            if (distance > -radius) {
                if (node.front > 0) {
                    stack.push_back(node.front);
                } else if (node.leaf[1] == leaf) {
                    return true;
                }
            }
            if (distance < radius) {
                if (node.back > 0) {
                    stack.push_back(node.back);
                } else if (node.leaf[0] == leaf) {
                    return true;
                }
            }
        }
        return false;
    }
}

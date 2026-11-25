#include "core/objects.h"
#include "core/vm.h"
#include "engine/actor.h"
#include "engine/bsp.h"
#include "engine/resources.h"
#include "engine/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dx {
    namespace {
        struct DVec {
            double x = 0;
            double y = 0;
            double z = 0;
        };

        struct DPlane {
            double x = 0;
            double y = 0;
            double z = 0;
            double w = 0;
        };

        DVec toD(const Vector& v)
        {
            return { v.x, v.y, v.z };
        }

        Vector toF(const DVec& v)
        {
            return { static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) };
        }

        double dot3(const DVec& a, const DVec& b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        DVec cross3(const DVec& a, const DVec& b)
        {
            return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
        }

        DVec normalize3(const DVec& v)
        {
            double length = std::sqrt(dot3(v, v));
            if (length < 1e-12) {
                return {};
            }
            return { v.x / length, v.y / length, v.z / length };
        }

        DPlane nodePlane(const UModel& model, int32_t index)
        {
            const Plane& p = model.nodes[static_cast<size_t>(index)].plane;
            return { p.x, p.y, p.z, p.w };
        }

        struct BrushTransform {
            Vector location;
            Coords axes;
            Vector scale { 1, 1, 1 };
            Vector prePivot;
        };

        BrushTransform brushTransform(UObject* a)
        {
            BrushTransform t;
            t.location = actor::Location(a);
            t.axes = rotationAxes(actor::Rotation(a));
            t.prePivot = actor::PrePivot(a);
            Vector* mainScale = reinterpret_cast<Vector*>(brush::MainScale(a));
            if (mainScale && mainScale->x != 0 && mainScale->y != 0 && mainScale->z != 0) {
                t.scale = *mainScale;
            }
            return t;
        }

        Vector brushNormalToWorld(const BrushTransform& t, const Vector& normal)
        {
            Vector scaled { normal.x / t.scale.x, normal.y / t.scale.y, normal.z / t.scale.z };
            return untransformVector(t.axes, scaled).normal();
        }

        using bsp::childOutside;
        using bsp::isCsg;

        DPlane tracePlane(const UModel& model, const BrushTransform* owner, int32_t index)
        {
            DPlane p = nodePlane(model, index);
            if (!owner) {
                return p;
            }
            Vector n = brushNormalToWorld(*owner, { static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) });
            Vector q { static_cast<float>(p.x * p.w), static_cast<float>(p.y * p.w), static_cast<float>(p.z * p.w) };
            Vector scaled { (q.x - owner->prePivot.x) * owner->scale.x, (q.y - owner->prePivot.y) * owner->scale.y, (q.z - owner->prePivot.z) * owner->scale.z };
            Vector world = untransformVector(owner->axes, scaled) + owner->location;
            return { n.x, n.y, n.z, static_cast<double>(n.dot(world)) };
        }

        struct BoxLineTrace {
            const UModel& model;
            const BrushTransform* owner;
            DVec start;
            DVec end;
            DVec extent;
            double hitTime = 2.0;
            DVec hitNormal;
            int32_t hitNode = -1;
            bool didHit = false;
            std::vector<DPlane> hulls;
            std::vector<int32_t> hullNodes;
            std::vector<int> flags;
            DVec min;
            DVec max;
            DVec localHit;
            int32_t localNode = -1;
            double t0 = 0;
            double t1 = 0;

            BoxLineTrace(const UModel& traceModel, const BrushTransform* traceOwner, const DVec& traceStart, const DVec& traceEnd, const DVec& traceExtent)
                : model(traceModel)
                , owner(traceOwner)
                , start(traceStart)
                , end(traceEnd)
                , extent(traceExtent)
            {
            }

            DPlane plane(int32_t index) const
            {
                return tracePlane(model, owner, index);
            }

            bool clipTo(const DPlane& hull, int32_t item)
            {
                double pushOut = std::abs(hull.x * extent.x) + std::abs(hull.y * extent.y) + std::abs(hull.z * extent.z);
                double d0 = hull.x * start.x + hull.y * start.y + hull.z * start.z - hull.w;
                double d1 = hull.x * end.x + hull.y * end.y + hull.z * end.z - hull.w;
                double adjD0 = d0 - pushOut;
                if (d0 > d1 && adjD0 >= -pushOut && adjD0 < 0) {
                    adjD0 = 0.0;
                }
                double t = adjD0 / (d0 - d1);
                if ((d0 - d1) < -0.00001) {
                    if (t < t1) {
                        t1 = t;
                    }
                } else if ((d0 - d1) > 0.00001) {
                    if (t > t0) {
                        t0 = t;
                        localHit = { hull.x, hull.y, hull.z };
                        localNode = item;
                    }
                } else if (d0 > pushOut && d1 > pushOut) {
                    return false;
                }
                return t0 < t1;
            }

            void setupHulls(const BspNode& node)
            {
                hulls.clear();
                hullNodes.clear();
                flags.clear();
                size_t index = static_cast<size_t>(node.collisionBound);
                while (index < model.leafHulls.size() && model.leafHulls[index] >= 0 && hulls.size() < 64) {
                    int32_t entry = model.leafHulls[index];
                    int32_t hullNode = entry & ~0x40000000;
                    DPlane hull = plane(hullNode);
                    if (entry & 0x40000000) {
                        hull = { -hull.x, -hull.y, -hull.z, -hull.w };
                    }
                    hulls.push_back(hull);
                    hullNodes.push_back(hullNode);
                    // Count a zero X or Y component as positive, but give zero Z neither sign.
                    // This gives edges against a floor or ceiling both X and Y bevels.
                    flags.push_back((hull.x < 0 ? 0x01 : 0x02) | (hull.y < 0 ? 0x04 : 0x08) | (hull.z < 0 ? 0x10 : hull.z > 0 ? 0x20
                                                                                                                              : 0));
                    index++;
                }
                float box[6] = {};
                size_t boxIndex = static_cast<size_t>(node.collisionBound) + hulls.size() + 1;
                for (size_t i = 0; i < 6 && boxIndex + i < model.leafHulls.size(); i++) {
                    int32_t raw = model.leafHulls[boxIndex + i];
                    std::memcpy(&box[i], &raw, 4);
                }
                min = { box[0], box[1], box[2] };
                max = { box[3], box[4], box[5] };
            }

            bool convolveEdge(size_t i, size_t j, const DVec& axis, int mask)
            {
                if (((flags[i] | flags[j]) & mask) != mask) {
                    return true;
                }
                DVec ni { hulls[i].x, hulls[i].y, hulls[i].z };
                DVec nj { hulls[j].x, hulls[j].y, hulls[j].z };
                if (dot3(cross3(axis, ni), cross3(axis, nj)) <= 0.001) {
                    return true;
                }
                DVec d = cross3(ni, nj);
                double dd = dot3(d, d);
                if (dd < 0.001 * 0.001) {
                    return t0 < t1;
                }
                DVec c1 = cross3(nj, d);
                DVec c2 = cross3(d, ni);
                DVec point { (hulls[i].w * c1.x + hulls[j].w * c2.x) / dd, (hulls[i].w * c1.y + hulls[j].w * c2.y) / dd, (hulls[i].w * c1.z + hulls[j].w * c2.z) / dd };
                DVec a = normalize3(cross3(axis, normalize3(d)));
                if (dot3(ni, a) < 0.0) {
                    a = { -a.x, -a.y, -a.z };
                }
                return clipTo({ a.x, a.y, a.z, dot3(point, a) }, -1);
            }

            bool clipPrimitive()
            {
                for (size_t i = 0; i < hulls.size(); i++) {
                    if (!clipTo(hulls[i], hullNodes[i])) {
                        return false;
                    }
                }
                if (!owner) {
                    if (!clipTo({ 0, 0, -1, 0.1 - min.z }, -1) || !clipTo({ 0, 0, 1, max.z + 0.1 }, -1) || !clipTo({ -1, 0, 0, 0.1 - min.x }, -1) || !clipTo({ 1, 0, 0, max.x - 0.1 }, -1) || !clipTo({ 0, -1, 0, 0.1 - min.y }, -1) || !clipTo({ 0, 1, 0, max.y - 0.1 }, -1)) {
                        return false;
                    }
                }
                for (size_t i = 0; i < hulls.size(); i++) {
                    for (size_t j = 0; j < i; j++) {
                        if (!convolveEdge(i, j, { 1, 0, 0 }, 0x03) || !convolveEdge(i, j, { 0, 1, 0 }, 0x0c) || !convolveEdge(i, j, { 0, 0, 1 }, 0x30)) {
                            return false;
                        }
                    }
                }
                return true;
            }

            void check(int32_t parent, int32_t nodeIndex, bool outside)
            {
                while (nodeIndex >= 0) {
                    const BspNode& node = model.nodes[static_cast<size_t>(nodeIndex)];
                    DPlane p = plane(nodeIndex);
                    double d0 = p.x * start.x + p.y * start.y + p.z * start.z - p.w;
                    double d1 = p.x * end.x + p.y * end.y + p.z * end.z - p.w;
                    double pushOut = std::abs(p.x * extent.x * 1.1) + std::abs(p.y * extent.y * 1.1) + std::abs(p.z * extent.z * 1.1);
                    bool use[2] = { d0 <= pushOut || d1 <= pushOut, d0 >= -pushOut || d1 >= -pushOut };
                    int frontFirst = d0 >= d1 ? 1 : 0;
                    if (use[frontFirst]) {
                        check(nodeIndex, frontFirst ? node.front : node.back, childOutside(node, frontFirst, outside));
                    }
                    if (!use[1 - frontFirst]) {
                        return;
                    }
                    parent = nodeIndex;
                    nodeIndex = frontFirst ? node.back : node.front;
                    outside = childOutside(node, 1 - frontFirst, outside);
                }
                const BspNode& parentNode = model.nodes[static_cast<size_t>(parent)];
                if (!outside && parentNode.collisionBound >= 0) {
                    setupHulls(parentNode);
                    t0 = -1.0;
                    t1 = hitTime;
                    localHit = {};
                    localNode = -1;
                    if (clipPrimitive() && t0 > -1.0 && t0 < t1 && t1 > 0.0) {
                        hitTime = t0;
                        hitNormal = localHit;
                        hitNode = localNode >= 0 ? localNode : parent;
                        didHit = true;
                    }
                }
            }
        };

        struct PointLineTrace {
            const UModel& model;
            const BrushTransform* owner;
            uint8_t nodeFlags;
            DVec location;
            int32_t item = -1;
            bool outOfCorner = false;

            PointLineTrace(const UModel& traceModel, const BrushTransform* traceOwner, uint8_t traceFlags)
                : model(traceModel)
                , owner(traceOwner)
                , nodeFlags(traceFlags)
            {
            }

            DPlane plane(int32_t index) const
            {
                return tracePlane(model, owner, index);
            }

            bool check(int32_t hitIndex, int32_t nodeIndex, DVec end, DVec start, bool outside)
            {
                while (nodeIndex >= 0) {
                    const BspNode& node = model.nodes[static_cast<size_t>(nodeIndex)];
                    DPlane p = plane(nodeIndex);
                    double dist1 = p.x * start.x + p.y * start.y + p.z * start.z - p.w;
                    double dist2 = p.x * end.x + p.y * end.y + p.z * end.z - p.w;
                    if (dist1 > -0.001 && dist2 > -0.001) {
                        outside = outside || isCsg(node, static_cast<uint8_t>(nodeFlags & ~0x10u));
                        nodeIndex = node.front;
                    } else if (dist1 < 0.001 && dist2 < 0.001) {
                        outside = outside && !isCsg(node, static_cast<uint8_t>(nodeFlags & ~0x10u));
                        nodeIndex = node.back;
                    } else {
                        double f = dist1 / (dist2 - dist1);
                        DVec middle { start.x + (start.x - end.x) * f, start.y + (start.y - end.y) * f, start.z + (start.z - end.z) * f };
                        int frontFirst = dist1 > 0.0 ? 1 : 0;
                        if (!check(hitIndex, frontFirst ? node.front : node.back, middle, start, childOutside(node, frontFirst, outside, nodeFlags))) {
                            return false;
                        }
                        outside = childOutside(node, 1 - frontFirst, outside, nodeFlags);
                        hitIndex = nodeIndex;
                        nodeIndex = frontFirst ? node.back : node.front;
                        start = middle;
                    }
                }
                if (!outside) {
                    if (outOfCorner || !(nodeFlags & 0x10)) {
                        location = start;
                        item = hitIndex;
                    } else {
                        outside = true;
                    }
                } else {
                    outOfCorner = true;
                }
                return outside;
            }
        };

        // UModel::PointCheck for a box: down to the
        // leaves the box reaches, and at one inside solid space against the hull of the node above
        // it. A plane separates the box when the box's push-out is no more than its distance in
        // front; the level's hulls also get their bounding box, and every hull gets the edge
        // bevels a box trace uses.
        struct BoxPointCheck {
            const UModel& model;
            const BrushTransform* owner;
            DVec center;
            DVec extent;
            uint8_t extraFlags = 0;
            std::vector<DPlane> hulls {};
            std::vector<int> flags {};

            DPlane plane(int32_t index) const
            {
                return tracePlane(model, owner, index);
            }

            bool separates(const DPlane& p) const
            {
                double pushOut = std::abs(extent.x * p.x) + std::abs(extent.y * p.y) + std::abs(extent.z * p.z);
                double distance = p.x * center.x + p.y * center.y + p.z * center.z - p.w;
                return pushOut <= distance;
            }

            bool bevelSeparates(size_t i, size_t j, const DVec& axis, int mask) const
            {
                if (((flags[i] | flags[j]) & mask) != mask) {
                    return false;
                }
                DVec ni { hulls[i].x, hulls[i].y, hulls[i].z };
                DVec nj { hulls[j].x, hulls[j].y, hulls[j].z };
                if (dot3(cross3(axis, ni), cross3(axis, nj)) <= 0.001) {
                    return false;
                }
                DVec d = cross3(ni, nj);
                double dd = dot3(d, d);
                if (dd < 1e-6) {
                    // The original goes on with a zero direction, which gives a plane that
                    // separates nothing.
                    return false;
                }
                DVec c1 = cross3(nj, d);
                DVec c2 = cross3(d, ni);
                DVec point { (hulls[i].w * c1.x + hulls[j].w * c2.x) / dd, (hulls[i].w * c1.y + hulls[j].w * c2.y) / dd, (hulls[i].w * c1.z + hulls[j].w * c2.z) / dd };
                DVec a = normalize3(cross3(axis, normalize3(d)));
                if (dot3(ni, a) < 0.0) {
                    a = { -a.x, -a.y, -a.z };
                }
                return separates({ a.x, a.y, a.z, dot3(point, a) });
            }

            bool leafOverlaps(const BspNode& node)
            {
                hulls.clear();
                flags.clear();
                size_t index = static_cast<size_t>(node.collisionBound);
                while (index < model.leafHulls.size() && model.leafHulls[index] >= 0 && hulls.size() < 64) {
                    int32_t entry = model.leafHulls[index];
                    DPlane hull = plane(entry & ~0x40000000);
                    if (entry & 0x40000000) {
                        hull = { -hull.x, -hull.y, -hull.z, -hull.w };
                    }
                    hulls.push_back(hull);
                    flags.push_back((hull.x < 0 ? 0x01 : 0x02) | (hull.y < 0 ? 0x04 : 0x08) | (hull.z < 0 ? 0x10 : hull.z > 0 ? 0x20
                                                                                                                              : 0));
                    index++;
                }
                for (const DPlane& hull : hulls) {
                    if (separates(hull)) {
                        return false;
                    }
                }
                if (!owner) {
                    double box[6] = {};
                    size_t boxIndex = static_cast<size_t>(node.collisionBound) + hulls.size() + 1;
                    for (size_t i = 0; i < 6 && boxIndex + i < model.leafHulls.size(); i++) {
                        int32_t raw = model.leafHulls[boxIndex + i];
                        float value = 0;
                        std::memcpy(&value, &raw, 4);
                        box[i] = static_cast<double>(value);
                    }
                    const DPlane bounds[6] = { { 0, 0, -1, 0.1 - box[2] }, { 0, 0, 1, box[5] + 0.1 }, { -1, 0, 0, 0.1 - box[0] }, { 1, 0, 0, box[3] - 0.1 }, { 0, -1, 0, 0.1 - box[1] }, { 0, 1, 0, box[4] - 0.1 } };
                    for (const DPlane& bound : bounds) {
                        if (separates(bound)) {
                            return false;
                        }
                    }
                }
                for (size_t i = 0; i < hulls.size(); i++) {
                    for (size_t j = 0; j < i; j++) {
                        if (bevelSeparates(i, j, { 1, 0, 0 }, 0x03) || bevelSeparates(i, j, { 0, 1, 0 }, 0x0c) || bevelSeparates(i, j, { 0, 0, 1 }, 0x30)) {
                            return false;
                        }
                    }
                }
                return true;
            }

            bool check(int32_t parent, int32_t nodeIndex, bool outside)
            {
                bool overlaps = false;
                while (nodeIndex >= 0) {
                    const BspNode& node = model.nodes[static_cast<size_t>(nodeIndex)];
                    DPlane p = plane(nodeIndex);
                    double pushOut = std::abs(extent.x * 1.1 * p.x) + std::abs(extent.y * 1.1 * p.y) + std::abs(extent.z * 1.1 * p.z);
                    double distance = p.x * center.x + p.y * center.y + p.z * center.z - p.w;
                    if (-pushOut < distance && check(nodeIndex, node.front, outside || isCsg(node, extraFlags))) {
                        overlaps = true;
                    }
                    outside = outside && !isCsg(node, extraFlags);
                    parent = nodeIndex;
                    nodeIndex = node.back;
                    if (pushOut < distance) {
                        return overlaps;
                    }
                }
                if (outside || parent < 0) {
                    return overlaps;
                }
                const BspNode& leaf = model.nodes[static_cast<size_t>(parent)];
                if (leaf.collisionBound < 0) {
                    return overlaps;
                }
                return leafOverlaps(leaf) || overlaps;
            }
        };

        bool modelOverlap(const UModel* model, const BrushTransform* owner, const Vector& center, const Vector& extent)
        {
            if (!model) {
                return false;
            }
            if (model->nodes.empty()) {
                return model->rootOutside == 0;
            }
            DVec point = toD(center);
            if (extent.zero()) {
                // A point is inside when the walk ends in solid space (a point exactly on a plane
                // counts as behind it).
                bool outside = model->rootOutside != 0;
                int32_t nodeIndex = 0;
                while (nodeIndex >= 0) {
                    const BspNode& node = model->nodes[static_cast<size_t>(nodeIndex)];
                    DPlane p = tracePlane(*model, owner, nodeIndex);
                    if (p.x * point.x + p.y * point.y + p.z * point.z - p.w <= 0.0) {
                        outside = outside && !isCsg(node, 0);
                        nodeIndex = node.back;
                    } else {
                        outside = outside || isCsg(node, 0);
                        nodeIndex = node.front;
                    }
                }
                return !outside;
            }
            BoxPointCheck check { *model, owner, point, toD(extent) };
            return check.check(0, 0, model->rootOutside != 0);
        }

        bool modelTrace(const UModel* model, const BrushTransform* owner, const Vector& start, const Vector& end, const Vector& extent, float& time, Vector& normal, int32_t* hitNode = nullptr, uint8_t nodeFlags = 0)
        {
            if (!model || model->nodes.empty()) {
                return false;
            }
            Vector delta = end - start;
            if (extent.zero()) {
                PointLineTrace trace(*model, owner, nodeFlags);
                if (trace.check(0, 0, toD(end), toD(start), model->rootOutside != 0)) {
                    return false;
                }
                Vector location = toF(trace.location);
                float rawTime = (location - start).dot(delta) / delta.dot(delta) - 0.5f / delta.size();
                // A zero-length trace inside solid geometry gives NaN. Treat it as a hit
                // at the start (time 0); rawTime >= 0 is false for NaN.
                time = !(rawTime >= 0.0f) ? 0.0f : std::min(rawTime, 1.0f);
                DPlane p = trace.plane(trace.item);
                normal = { static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) };
                // UModel::LineCheck turns a level hit's normal to face back
                // along the trace; a brush's normal is left as its plane gives it.
                if (!owner && normal.dot(delta) > 0.0f) {
                    normal = normal * -1.0f;
                }
                if (hitNode) {
                    *hitNode = trace.item;
                }
                return true;
            }
            BoxLineTrace trace(*model, owner, toD(start), toD(end), toD(extent));
            trace.check(0, 0, model->rootOutside != 0);
            if (!trace.didHit) {
                return false;
            }
            float dist = delta.size();
            time = std::clamp(static_cast<float>(trace.hitTime) - std::max(0.1f, 0.1f / dist), 0.0f, 1.0f);
            if (time == 1.0f) {
                return false;
            }
            normal = toF(trace.hitNormal);
            if (hitNode) {
                *hitNode = trace.hitNode;
            }
            return true;
        }

        bool brushTrace(UObject* a, const Vector& start, const Vector& end, const Vector& extent, float& time, Vector& normal, int32_t* hitNode = nullptr)
        {
            UModel* model = cast<UModel>(actor::Brush(a));
            if (!model) {
                return false;
            }
            BrushTransform t = brushTransform(a);
            return modelTrace(model, &t, start, end, extent, time, normal, hitNode);
        }

        bool brushOverlap(UObject* a, const Vector& center, const Vector& extent)
        {
            UModel* model = cast<UModel>(actor::Brush(a));
            if (!model) {
                return false;
            }
            BrushTransform t = brushTransform(a);
            return modelOverlap(model, &t, center, extent);
        }

        bool cylinderTrace(UObject* target, const Vector& start, const Vector& end, const Vector& extent, float& time, Vector& normal)
        {
            Vector center = actor::Location(target);
            float radius = actor::CollisionRadius(target) + extent.x;
            float height = actor::CollisionHeight(target) + extent.z;
            if ((start.x > center.x + radius && end.x > center.x + radius) || (start.x < center.x - radius && end.x < center.x - radius)) {
                return false;
            }
            if ((start.y > center.y + radius && end.y > center.y + radius) || (start.y < center.y - radius && end.y < center.y - radius)) {
                return false;
            }
            float topZ = center.z + height;
            float botZ = center.z - height;
            if ((start.z > topZ && end.z > topZ) || (start.z < botZ && end.z < botZ)) {
                return false;
            }
            float t0 = 0;
            float t1 = 1;
            Vector hitNormal;
            if (start.z > topZ && end.z < topZ) {
                float t = (topZ - start.z) / (end.z - start.z);
                if (t > t0) {
                    t0 = t;
                    hitNormal = { 0, 0, 1 };
                }
            } else if (start.z < topZ && end.z > topZ) {
                t1 = std::min(t1, (topZ - start.z) / (end.z - start.z));
            }
            if (start.z < botZ && end.z > botZ) {
                float t = (botZ - start.z) / (end.z - start.z);
                if (t > t0) {
                    t0 = t;
                    hitNormal = { 0, 0, -1 };
                }
            } else if (start.z > botZ && end.z < botZ) {
                t1 = std::min(t1, (botZ - start.z) / (end.z - start.z));
            }
            if (t0 >= t1) {
                return false;
            }
            float kx = start.x - center.x;
            float ky = start.y - center.y;
            float vx = end.x - start.x;
            float vy = end.y - start.y;
            float a = vx * vx + vy * vy;
            float b = 2.0f * (kx * vx + ky * vy);
            float c = kx * kx + ky * ky - radius * radius;
            float discriminant = b * b - 4.0f * a * c;
            if (c < 1.0f && start.z > botZ && start.z < topZ) {
                float dir = vx * kx + vy * ky;
                if (dir < -0.1f) {
                    time = 0;
                    normal = Vector { kx, ky, 0 }.normal();
                    return true;
                }
                return false;
            }
            if (discriminant < 0) {
                return false;
            }
            if (a < 1e-8f) {
                if (c > 0) {
                    return false;
                }
            } else {
                discriminant = std::sqrt(discriminant);
                float r2a = 0.5f / a;
                t1 = std::min(t1, (discriminant - b) * r2a);
                float t = -(discriminant + b) * r2a;
                if (t > t0) {
                    t0 = t;
                    hitNormal = Vector { kx + vx * t0, ky + vy * t0, 0 }.normal();
                }
                if (t0 >= t1) {
                    return false;
                }
            }
            time = std::clamp(t0 - 0.001f, 0.0f, 1.0f);
            normal = hitNormal;
            return true;
        }

        bool actorCollides(const World& world, UObject* other, UObject* source, uint32_t flags)
        {
            if (!other || other == source || other->destroyed || !actor::bCollideActors(other)) {
                return false;
            }
            if (other == world.levelInfo) {
                return false;
            }
            int guard = 0;
            for (UObject* owner = source ? actor::Owner(source) : nullptr; owner && guard < 64; owner = actor::Owner(owner), guard++) {
                if (owner == other) {
                    return false;
                }
            }
            if (isMover(other)) {
                return (flags & TRACE_Movers) != 0;
            }
            if (isBrush(other)) {
                return false;
            }
            if (isPawn(other)) {
                return (flags & TRACE_Pawns) != 0;
            }
            if (flags & TRACE_OnlyProjActor) {
                return actor::bProjTarget(other) || (actor::bBlockActors(other) && actor::bBlockPlayers(other));
            }
            if (flags & TRACE_Blocking) {
                return source ? isBlockedBy(world, source, other) : (actor::bBlockActors(other) != 0 || actor::bBlockPlayers(other) != 0);
            }
            return (flags & TRACE_Others) != 0;
        }
    }

    int32_t World::pointLeaf(const Vector& location)
    {
        int32_t leaf = -1;
        zoneAt(location, &leaf, nullptr);
        return leaf;
    }

    UObject* World::zoneAt(const Vector& location, int32_t* leaf, uint8_t* zoneNumber)
    {
        UModel* model = level ? level->model : nullptr;
        if (leaf) {
            *leaf = -1;
        }
        if (zoneNumber) {
            *zoneNumber = 0;
        }
        if (!model || model->nodes.empty()) {
            return levelInfo;
        }
        int32_t index = 0;
        int guard = 0;
        while (guard++ < 10000) {
            const BspNode& node = model->nodes[static_cast<size_t>(index)];
            float distance = node.plane.x * location.x + node.plane.y * location.y + node.plane.z * location.z - node.plane.w;
            // UModel::PointRegion puts a point on the plane in front, the side a
            // surface faces: an actor exactly on a floor or wall is in the room.
            int side = distance >= 0 ? 1 : 0;
            int32_t next = side ? node.front : node.back;
            if (next < 0) {
                uint8_t zone = node.zone[side];
                if (leaf) {
                    *leaf = node.leaf[side];
                }
                if (zoneNumber) {
                    *zoneNumber = zone;
                }
                UObject* zoneActor = zone < model->zones.size() ? model->zones[zone].zoneActor : nullptr;
                return zoneActor ? zoneActor : levelInfo;
            }
            index = next;
        }
        return levelInfo;
    }

    void World::setLocationZone(UObject* a, const Vector& location, bool test)
    {
        if (!a || a->destroyed) {
            return;
        }
        if (a == levelInfo) {
            region(a) = { levelInfo, -1, 0 };
            return;
        }
        PointRegion newRegion;
        newRegion.zone = zoneAt(location, &newRegion.iLeaf, &newRegion.zoneNumber);
        PointRegion& oldRegion = region(a);
        if (newRegion.zone != oldRegion.zone && test) {
            oldRegion = newRegion;
        } else if (newRegion.zone != oldRegion.zone) {
            UObject* oldZone = oldRegion.zone;
            if (oldZone) {
                vm::Call(oldZone, Name("ActorLeaving")).obj(a).run();
            }
            vm::Call(a, Name("ZoneChange")).obj(newRegion.zone).run();
            if (a->destroyed) {
                return;
            }
            region(a) = newRegion;
            vm::Call(newRegion.zone, Name("ActorEntered")).obj(a).run();
        } else {
            oldRegion = newRegion;
        }
        if (isPawn(a) && !a->destroyed) {
            PointRegion& foot = *reinterpret_cast<PointRegion*>(pawn::FootRegion(a));
            PointRegion newFoot;
            newFoot.zone = zoneAt(actor::Location(a) - Vector { 0, 0, actor::CollisionHeight(a) }, &newFoot.iLeaf, &newFoot.zoneNumber);
            if (newFoot.zone != foot.zone && !test) {
                vm::Call(a, Name("FootZoneChange")).obj(newFoot.zone).run();
            }
            *reinterpret_cast<PointRegion*>(pawn::FootRegion(a)) = newFoot;
            PointRegion& head = *reinterpret_cast<PointRegion*>(pawn::HeadRegion(a));
            PointRegion newHead;
            newHead.zone = zoneAt(actor::Location(a) + Vector { 0, 0, pawn::EyeHeight(a) }, &newHead.iLeaf, &newHead.zoneNumber);
            if (newHead.zone != head.zone && !test) {
                vm::Call(a, Name("HeadZoneChange")).obj(newHead.zone).run();
            }
            *reinterpret_cast<PointRegion*>(pawn::HeadRegion(a)) = newHead;
        }
    }

    namespace {
        bool signBitClear(float value)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return (bits & 0x80000000u) == 0;
        }

        bool fastLineNode(const UModel& model, int32_t nodeIndex, Vector end, Vector start, bool outside)
        {
            while (nodeIndex >= 0) {
                const BspNode& node = model.nodes[static_cast<size_t>(nodeIndex)];
                float dist1 = node.plane.x * start.x + node.plane.y * start.y + node.plane.z * start.z - node.plane.w;
                float dist2 = node.plane.x * end.x + node.plane.y * end.y + node.plane.z * end.z - node.plane.w;
                bool notCsg = (node.nodeFlags & 0x01) != 0;
                bool g1 = signBitClear(dist1);
                bool g2 = signBitClear(dist2);
                if (g1 != g2) {
                    float alpha = dist1 / (dist1 - dist2);
                    Vector middle = start + (end - start) * alpha;
                    if (!fastLineNode(model, g2 ? node.front : node.back, middle, end, notCsg ? outside : g2)) {
                        return false;
                    }
                    end = middle;
                }
                outside = notCsg ? outside : g1;
                nodeIndex = g1 ? node.front : node.back;
            }
            return outside;
        }
    }

    bool World::fastLineCheck(const Vector& end, const Vector& start)
    {
        UModel* model = level ? level->model : nullptr;
        if (!model) {
            return true;
        }
        if (model->nodes.empty()) {
            return model->rootOutside != 0;
        }
        return fastLineNode(*model, 0, end, start, model->rootOutside != 0);
    }

    bool World::bspLineCheck(CheckResult& hit, const Vector& start, const Vector& end, const Vector& extent, uint8_t nodeFlags)
    {
        float time = 1;
        Vector normal;
        int32_t node = -1;
        UModel* model = level ? level->model : nullptr;
        if (!modelTrace(model, nullptr, start, end, extent, time, normal, &node, nodeFlags)) {
            return false;
        }
        hit.actor = levelInfo;
        hit.time = time;
        hit.normal = normal;
        hit.location = start + (end - start) * time;
        hit.item = node;
        if (node >= 0 && static_cast<size_t>(node) < model->nodes.size()) {
            int32_t surf = model->nodes[static_cast<size_t>(node)].surf;
            if (surf >= 0 && static_cast<size_t>(surf) < model->surfs.size()) {
                hit.material = model->surfs[static_cast<size_t>(surf)].texture;
            }
        }
        return true;
    }

    std::vector<CheckResult> World::multiLineCheck(const Vector& start, const Vector& end, const Vector& extent, UObject* source, uint32_t flags, uint8_t nodeFlags)
    {
        std::vector<CheckResult> hits;
        Vector actorEnd = end;
        float dilation = 1.0f;
        bool onlyMovers = false;
        if (flags & (TRACE_Level | TRACE_LevelGeometry)) {
            CheckResult hit;
            if (bspLineCheck(hit, start, end, extent, nodeFlags)) {
                // ULevel::MultiLineCheck: actors are checked up to 5 units
                // past the level hit, and only movers when that hit is within the first 1% of the
                // trace and under 30 units away. Without the distance test, a long trace that ends on
                // a wall would miss whatever stands in front of it.
                float distance = (hit.location - start).size();
                dilation = std::min(1.0f, (distance + 5.0f) * hit.time / (distance + 0.0001f));
                actorEnd = start + (end - start) * dilation;
                onlyMovers = hit.time < 0.01f && distance < 30.0f;
                hits.push_back(hit);
            }
        }
        if (flags & (TRACE_Pawns | TRACE_Movers | TRACE_Others | TRACE_OnlyProjActor | TRACE_Blocking)) {
            Vector boxMin { std::min(start.x, actorEnd.x) - extent.x, std::min(start.y, actorEnd.y) - extent.y, std::min(start.z, actorEnd.z) - extent.z };
            Vector boxMax { std::max(start.x, actorEnd.x) + extent.x, std::max(start.y, actorEnd.y) + extent.y, std::max(start.z, actorEnd.z) + extent.z };
            for (UObject* other : collisionHash.query(boxMin, boxMax)) {
                if (!other || other->destroyed || !actor::bCollideActors(other)) {
                    continue;
                }
                if (onlyMovers && !isMover(other)) {
                    continue;
                }
                bool brush = isBrush(other);
                if (!brush) {
                    Vector location = actor::Location(other);
                    float radius = actor::CollisionRadius(other);
                    float height = actor::CollisionHeight(other);
                    if (location.x + radius < boxMin.x || location.x - radius > boxMax.x || location.y + radius < boxMin.y || location.y - radius > boxMax.y || location.z + height < boxMin.z || location.z - height > boxMax.z) {
                        continue;
                    }
                }
                if (!actorCollides(*this, other, source, flags)) {
                    continue;
                }
                float time = 1;
                Vector normal;
                bool collided = false;
                int32_t node = -1;
                if (brush) {
                    collided = brushTrace(other, start, actorEnd, extent, time, normal, &node);
                } else {
                    collided = cylinderTrace(other, start, actorEnd, extent, time, normal);
                }
                if (collided) {
                    CheckResult hit;
                    hit.actor = other;
                    hit.time = time * dilation;
                    hit.normal = normal;
                    hit.location = start + (actorEnd - start) * time;
                    hit.item = node;
                    if (!hits.empty() && hits.front().actor == levelInfo && isMover(other) && (hit.normal - hits.front().normal).sizeSquared() < 1e-6f && (hit.location - hits.front().location).sizeSquared() < 4.0f) {
                        Vector direction = (actorEnd - start).normal();
                        hit.location = hits.front().location - direction * 2.0f;
                        hit.time = (hit.location - start).size() / (end - start).size();
                    }
                    hits.push_back(hit);
                    // The original keeps at most 64 results, the level hit included.
                    if (hits.size() >= 64) {
                        break;
                    }
                }
            }
        }
        std::stable_sort(hits.begin(), hits.end(), [](const CheckResult& a, const CheckResult& b) {
            return a.time < b.time;
        });
        return hits;
    }

    bool World::lineCheck(CheckResult& hit, const Vector& start, const Vector& end, const Vector& extent, UObject* source, uint32_t flags)
    {
        hit = CheckResult();
        hit.location = end;
        std::vector<CheckResult> hits = multiLineCheck(start, end, extent, source, flags);
        if (hits.empty()) {
            return false;
        }
        hit = hits.front();
        return true;
    }

    bool World::pointCheck(CheckResult& hit, const Vector& location, const Vector& extent, UObject* source, bool checkActors)
    {
        hit = CheckResult();
        if (modelOverlap(level ? level->model : nullptr, nullptr, location, extent)) {
            hit.actor = levelInfo;
            hit.location = location;
            hit.time = 0;
            return true;
        }
        if (!checkActors) {
            return false;
        }
        for (UObject* other : collisionHash.query(location - extent, location + extent)) {
            if (!actorCollides(*this, other, source, TRACE_Pawns | TRACE_Movers | TRACE_Others)) {
                continue;
            }
            bool overlap = false;
            if (isBrush(other)) {
                overlap = brushOverlap(other, location, extent);
            } else {
                Vector d = actor::Location(other) - location;
                float radius = actor::CollisionRadius(other) + extent.x;
                float height = actor::CollisionHeight(other) + extent.z;
                overlap = d.x * d.x + d.y * d.y < radius * radius && std::abs(d.z) < height;
            }
            if (overlap) {
                hit.actor = other;
                hit.location = location;
                hit.time = 0;
                return true;
            }
        }
        return false;
    }

    bool World::findSpot(const Vector& extent, Vector& location, bool checkActors, bool assumeFit)
    {
        CheckResult hit;
        if (extent.zero()) {
            return !pointCheck(hit, location, extent, nullptr, checkActors);
        }
        if (assumeFit && !pointCheck(hit, location, extent, nullptr, checkActors)) {
            return true;
        }
        Vector adjusted = location;
        auto adjust = [this](Vector& spot, const Vector& destination, float length) {
            CheckResult trace;
            if (lineCheck(trace, spot, destination, {}, nullptr, TRACE_World) && trace.time < 1.0f) {
                spot = spot + trace.normal * ((1.05f - trace.time) * length);
            }
        };
        for (int i = -1; i < 2; i += 2) {
            float sign = static_cast<float>(i);
            adjust(adjusted, adjusted + Vector { sign * extent.x, 0, 0 }, extent.x);
            adjust(adjusted, adjusted + Vector { 0, sign * extent.y, 0 }, extent.y);
            adjust(adjusted, adjusted + Vector { 0, 0, sign * extent.z }, extent.z);
        }
        if (!pointCheck(hit, adjusted, extent, nullptr, checkActors)) {
            location = adjusted;
            return true;
        }
        float length = extent.size() + 2.0f;
        for (int i = -1; i < 2; i += 2) {
            for (int j = -1; j < 2; j += 2) {
                for (int k = -1; k < 2; k += 2) {
                    adjust(adjusted, adjusted + Vector { static_cast<float>(i) * extent.x, static_cast<float>(j) * extent.y, static_cast<float>(k) * extent.z }, length);
                }
            }
        }
        if ((adjusted - location).sizeSquared() > 1.5f * extent.sizeSquared()) {
            return false;
        }
        if (!pointCheck(hit, adjusted, extent, nullptr, checkActors)) {
            location = adjusted;
            return true;
        }
        return false;
    }

    bool World::encroaches(UObject* a, const Vector& location, bool touchNotify)
    {
        if (!actor::bCollideActors(a) && !actor::bBlockActors(a) && !actor::bBlockPlayers(a) && !isMovingBrush(a)) {
            return false;
        }
        std::vector<UObject*> encroached;
        Vector extent = cylinderExtent(a);
        const std::vector<UObject*>& candidates = isMovingBrush(a) ? actors : collisionHash.query(location - extent, location + extent);
        for (UObject* other : candidates) {
            if (!other || other == a || other->destroyed || other == levelInfo || !actor::bCollideActors(other)) {
                continue;
            }
            bool overlap = false;
            if (isMovingBrush(a)) {
                if (isBrush(other)) {
                    continue;
                }
                Vector saved = actor::Location(a);
                actor::Location(a) = location;
                overlap = brushOverlap(a, actor::Location(other), cylinderExtent(other));
                actor::Location(a) = saved;
            } else if (isBrush(other)) {
                overlap = brushOverlap(other, location, cylinderExtent(a));
            } else {
                Vector d = actor::Location(other) - location;
                float radius = actor::CollisionRadius(other) + actor::CollisionRadius(a);
                float height = actor::CollisionHeight(other) + actor::CollisionHeight(a);
                overlap = d.x * d.x + d.y * d.y < radius * radius && std::abs(d.z) < height;
            }
            if (overlap) {
                encroached.push_back(other);
            }
        }
        for (UObject* other : encroached) {
            if (!isBlockedBy(*this, a, other)) {
                continue;
            }
            if (isMovingBrush(a) && !isMovingBrush(other)) {
                Vector push = location - actor::Location(a);
                Vector old = actor::Location(other);
                moveSmooth(other, push);
                Vector saved = actor::Location(a);
                actor::Location(a) = location;
                bool still = brushOverlap(a, actor::Location(other), cylinderExtent(other));
                actor::Location(a) = saved;
                if (!still) {
                    continue;
                }
                actor::Location(a) = location;
                moveSmooth(other, push * -1.0f);
                actor::Location(a) = saved;
                (void)old;
            }
            vm::Call call(a, Name("EncroachingOn"));
            if (call.valid()) {
                call.obj(other).run();
                uint8_t* result = call.returnValue();
                if (result && *reinterpret_cast<uint32_t*>(result)) {
                    return true;
                }
            }
        }
        if (touchNotify) {
            for (int i = 0; i < 4; i++) {
                UObject* touching = actor::Touching(a, i);
                if (touching && !isOverlapping(this, a, touching)) {
                    endTouch(a, touching, false);
                }
            }
        }
        for (UObject* other : encroached) {
            if (isBlockedBy(*this, a, other)) {
                vm::Call(other, Name("EncroachedBy")).obj(a).run();
            } else if (touchNotify) {
                beginTouch(a, other);
            }
        }
        return false;
    }

    bool World::farMoveActor(UObject* a, const Vector& destination, bool test, bool noCheck)
    {
        if (actor::bStatic(a) || !actor::bMovable(a)) {
            return false;
        }
        Vector newLocation = destination;
        bool result = true;
        // ULevel::FarMoveActor: unlike Epic's version, bNoCheck skips FindSpot for
        // bCollideWhenPlacing actors too.
        if (!noCheck && (actor::bCollideWorld(a) || actor::bCollideWhenPlacing(a))) {
            result = findSpot(cylinderExtent(a), newLocation, false, false);
        }
        if (result && !test && !noCheck) {
            result = !encroaches(a, newLocation, !objects::editorMode());
            if (a->destroyed) {
                return false;
            }
        }
        if (result) {
            if (!test) {
                if (actor::StandingCount(a) > 0) {
                    for (size_t i = 0; i < actors.size(); i++) {
                        UObject* other = actors[i];
                        if (other && actor::Base(other) == a) {
                            setBase(other, nullptr, true);
                        }
                    }
                }
                actor::bJustTeleported(a) = 1;
            }
            actor::Location(a) = newLocation;
            actor::OldLocation(a) = newLocation;
            collisionHash.update(a);
            setLocationZone(a, newLocation, test);
        }
        return result;
    }

    bool World::moveActor(UObject* a, const Vector& delta, const Rotator& rotation, CheckResult& hit, bool test, bool ignorePawns, bool ignoreBases, bool noFail)
    {
        if (!a || a->destroyed || actor::bStatic(a) || !actor::bMovable(a)) {
            return false;
        }
        hit = CheckResult();
        bool movingBrush = isMovingBrush(a);
        // ULevel::MoveActor treats a move as none when each component is
        // under 0.0001.
        if (std::abs(delta.x) < 0.0001f && std::abs(delta.y) < 0.0001f && std::abs(delta.z) < 0.0001f) {
            if (rotation == actor::Rotation(a)) {
                return true;
            }
            if (!actor::StandingCount(a) && !movingBrush) {
                actor::Rotation(a) = rotation;
                return true;
            }
        }
        float deltaSize = delta.size();
        Vector deltaDir = deltaSize > 0 ? delta / deltaSize : Vector {};
        const float testAdjust = 2.0f;
        Vector testDelta = delta + deltaDir * testAdjust;
        bool maybeTouched = false;
        std::vector<CheckResult> hits;
        Vector location = actor::Location(a);
        if ((actor::bCollideActors(a) || actor::bCollideWorld(a)) && !movingBrush && deltaSize > 0) {
            uint32_t flags = 0;
            if (actor::bCollideActors(a)) {
                flags |= TRACE_Pawns | TRACE_Movers | TRACE_Others;
            }
            if (actor::bCollideWorld(a)) {
                flags |= TRACE_Level | TRACE_LevelGeometry | TRACE_Movers;
            }
            hits = multiLineCheck(location, location + testDelta, cylinderExtent(a), nullptr, flags);
            hits.erase(std::remove_if(hits.begin(), hits.end(), [a](const CheckResult& candidate) {
                return candidate.actor == a;
            }),
                hits.end());
            if (actor::bCollideWorld(a) || actor::bBlockActors(a) || actor::bBlockPlayers(a)) {
                for (const CheckResult& candidate : hits) {
                    UObject* other = candidate.actor;
                    bool pawnLike = isPawn(other) || other->isA(engineClasses().decoration);
                    if ((!ignorePawns || actor::bStatic(other) || !pawnLike) && (!ignoreBases || !isBasedOn(a, other)) && !isBasedOn(other, a)) {
                        maybeTouched = true;
                        if (isBlockedBy(*this, a, other)) {
                            hit = candidate;
                            break;
                        }
                    }
                }
            }
        }
        Vector finalDelta = delta;
        if (hit.time < 1.0f && !noFail) {
            float finalSize = (deltaSize + testAdjust) * hit.time;
            if (finalSize <= testAdjust) {
                finalDelta = {};
                hit.time = 0;
            } else {
                finalDelta = testDelta * hit.time - deltaDir * testAdjust;
                hit.time = (finalSize - testAdjust) / deltaSize;
            }
        }
        if (actor::StandingCount(a) && !test) {
            Rotator oldRotation = actor::Rotation(a);
            int32_t deltaYaw = rotation.yaw - oldRotation.yaw;
            for (UObject* other : std::vector<UObject*>(actors)) {
                if (!other || other->destroyed || actor::Base(other) != a) {
                    continue;
                }
                Vector rotationMotion;
                if (!(rotation == oldRotation)) {
                    // Deus Ex's ReduceAngle keeps the yaw to multiples of 4, as & 0xfffc.
                    Rotator reduced { 0, (rotation.yaw & 0xfffc) - (oldRotation.yaw & 0xfffc), 0 };
                    Vector pointer = actor::Location(a) - actor::Location(other);
                    Coords spin = rotationAxes(reduced);
                    // Apply the inverse rotation to the offset between the two actors.
                    rotationMotion = pointer - untransformVector(spin, pointer);
                }
                CheckResult otherHit;
                Rotator otherRotation = actor::Rotation(other);
                otherRotation.yaw += deltaYaw;
                moveActor(other, finalDelta + rotationMotion, otherRotation, otherHit, false, false, true, false);
                if (isPawn(other)) {
                    pawn::ViewRotation(other).yaw += deltaYaw;
                }
            }
        }
        if (!test && !noFail && !isPawn(a)) {
            Vector saved = actor::Location(a);
            Rotator savedRotation = actor::Rotation(a);
            actor::Rotation(a) = rotation;
            bool blocked = encroaches(a, saved + finalDelta);
            actor::Rotation(a) = savedRotation;
            if (blocked) {
                return false;
            }
        }
        actor::Location(a) = actor::Location(a) + finalDelta;
        actor::Rotation(a) = rotation;
        collisionHash.update(a);
        if (!test) {
            UObject* hitActor = hit.actor;
            if (hitActor && hitActor != levelInfo && !isBasedOn(a, hitActor)) {
                vm::Call(hitActor, Name("Bump")).obj(a).run();
                if (!a->destroyed) {
                    vm::Call(a, Name("Bump")).obj(hitActor).run();
                }
            }
            if (!a->destroyed && (maybeTouched || !actor::bBlockActors(a) || !actor::bBlockPlayers(a))) {
                for (const CheckResult& candidate : hits) {
                    if (candidate.time >= hit.time) {
                        break;
                    }
                    UObject* other = candidate.actor;
                    if (other == levelInfo || other->destroyed || a->destroyed) {
                        continue;
                    }
                    if (!isBasedOn(other, a) && (!ignoreBases || !isBasedOn(a, other)) && !isBlockedBy(*this, a, other)) {
                        beginTouch(a, other);
                    }
                }
            }
            if (!a->destroyed) {
                for (int i = 0; i < 4; i++) {
                    UObject* touching = actor::Touching(a, i);
                    if (touching && !isOverlapping(this, a, touching)) {
                        endTouch(a, touching, false);
                    }
                }
            }
        }
        if (!a->destroyed) {
            setLocationZone(a, actor::Location(a), test);
        }
        return hit.time > 0.0f;
    }

    bool moveSmooth(UObject* a, const Vector& delta)
    {
        if (!World::hasActive()) {
            return false;
        }
        World& world = World::active();
        CheckResult hit;
        bool moved = world.moveActor(a, delta, actor::Rotation(a), hit, false, false, false, false);
        if (hit.time < 1.0f && !a->destroyed) {
            Vector adjusted = (delta - hit.normal * delta.dot(hit.normal)) * (1.0f - hit.time);
            if (delta.dot(adjusted) >= 0) {
                Vector oldNormal = hit.normal;
                Vector desired = delta.normal();
                world.moveActor(a, adjusted, actor::Rotation(a), hit, false, false, false, false);
                if (hit.time < 1.0f && !a->destroyed) {
                    vm::Call(a, Name("HitWall")).v(hit.normal).obj(hit.actor).run();
                    if (oldNormal.dot(hit.normal) <= 0) {
                        Vector direction = hit.normal.cross(oldNormal).normal();
                        adjusted = direction * (adjusted.dot(direction) * (1.0f - hit.time));
                        if (desired.dot(adjusted) < 0) {
                            adjusted = adjusted * -1.0f;
                        }
                    } else {
                        adjusted = (adjusted - hit.normal * adjusted.dot(hit.normal)) * (1.0f - hit.time);
                        if (adjusted.dot(desired) <= 0) {
                            adjusted = {};
                        }
                    }
                    world.moveActor(a, adjusted, actor::Rotation(a), hit, false, false, false, false);
                }
            }
        }
        return moved;
    }
}

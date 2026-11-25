#include "engine/level_geometry.h"

#include "core/derived_cache.h"
#include "core/objects.h"
#include "engine/actor.h"
#include "engine/bsp.h"
#include "engine/props_engine.h"
#include "engine/resources.h"

namespace dx::levelgeometry {
    bool Surface::backdrop() const
    {
        return (flags & bsp::PF_FakeBackdrop) != 0;
    }

    bool Surface::portal() const
    {
        return (flags & bsp::PF_Portal) != 0;
    }

    bool Surface::invisible() const
    {
        return (flags & bsp::PF_Invisible) != 0;
    }

    std::vector<Triangle> Shape::triangles() const
    {
        std::vector<Triangle> fans;
        for (size_t p = 0; p < polygons.size(); p++) {
            const Polygon& polygon = polygons[p];
            for (uint32_t i = 1; i + 1 < polygon.pointCount; i++) {
                Triangle triangle;
                triangle.points = { points[polygon.firstPoint], points[polygon.firstPoint + i], points[polygon.firstPoint + i + 1] };
                triangle.polygon = static_cast<int32_t>(p);
                triangle.surface = polygon.surface;
                fans.push_back(triangle);
            }
        }
        return fans;
    }

    bool MoverTransform::operator==(const MoverTransform& other) const
    {
        return actor == other.actor && location.x == other.location.x && location.y == other.location.y && location.z == other.location.z && rotation == other.rotation && scale.x == other.scale.x && scale.y == other.scale.y && scale.z == other.scale.z && prePivot.x == other.prePivot.x && prePivot.y == other.prePivot.y && prePivot.z == other.prePivot.z;
    }

    Vector MoverTransform::place(const Vector& local) const
    {
        return location + untransformVector(rotationAxes(rotation), (local - prePivot) * scale);
    }

    Surface describeSurface(UObject* texture, uint32_t flags)
    {
        Surface surface;
        surface.texture = texture;
        surface.flags = flags;
        if (texture) {
            objects::ensureLoaded(texture);
            surface.textureName = texture->name.str();
            surface.group = texture->outer ? texture->outer->name.str() : "";
            if (texture->data) {
                if (texture::bMasked(texture)) {
                    surface.flags |= PF_Masked;
                }
                if (texture::bTransparent(texture)) {
                    surface.flags |= PF_Translucent;
                }
                if (texture::bModulate(texture)) {
                    surface.flags |= PF_Modulated;
                }
            }
        }
        return surface;
    }

    std::vector<MoverTransform> moverTransforms(ULevel* level)
    {
        std::vector<MoverTransform> transforms;
        if (!level) {
            return transforms;
        }
        for (size_t i = 0; i < level->actors.size(); i++) {
            UObject* actor = level->actors[i];
            if (!actor || actor->destroyed || !actor->data || !isMover(actor) || !actor::Brush(actor)) {
                continue;
            }
            MoverTransform transform;
            transform.actor = static_cast<int32_t>(i);
            transform.location = actor::Location(actor);
            transform.rotation = actor::Rotation(actor);
            Vector scale = *reinterpret_cast<Vector*>(brush::MainScale(actor));
            transform.scale = scale.x == 0 && scale.y == 0 && scale.z == 0 ? Vector { 1, 1, 1 } : scale;
            transform.prePivot = actor::PrePivot(actor);
            transforms.push_back(transform);
        }
        return transforms;
    }

    Shape brushShape(UModel* brush)
    {
        Shape shape;
        if (!brush || !brush->polys) {
            return shape;
        }
        objects::ensureLoaded(brush->polys);
        const std::vector<Poly>& polys = brush->polys->polys;
        for (size_t i = 0; i < polys.size(); i++) {
            const Poly& poly = polys[i];
            Surface surface = describeSurface(poly.texture, poly.polyFlags);
            surface.source = static_cast<int32_t>(i);
            shape.surfaces.push_back(std::move(surface));
            Polygon polygon;
            polygon.surface = static_cast<int32_t>(shape.surfaces.size() - 1);
            polygon.normal = poly.normal;
            polygon.firstPoint = static_cast<uint32_t>(shape.points.size());
            polygon.pointCount = static_cast<uint32_t>(poly.vertices.size());
            shape.points.insert(shape.points.end(), poly.vertices.begin(), poly.vertices.end());
            shape.polygons.push_back(polygon);
        }
        return shape;
    }

    namespace {
        void hashVector(derivedcache::Hash& hash, const Vector& v)
        {
            hash.add(v.x);
            hash.add(v.y);
            hash.add(v.z);
        }

        void hashShape(derivedcache::Hash& hash, const Shape& shape)
        {
            hash.add(static_cast<uint64_t>(shape.surfaces.size()));
            for (const Surface& surface : shape.surfaces) {
                hash.add(surface.source);
                hash.text(surface.textureName);
                hash.text(surface.group);
                hash.add(surface.flags);
            }
            hash.add(static_cast<uint64_t>(shape.polygons.size()));
            for (const Polygon& polygon : shape.polygons) {
                hash.add(polygon.surface);
                hash.add(polygon.node);
                hash.add(polygon.skyZone);
                hashVector(hash, polygon.normal);
                hash.add(polygon.pointCount);
                for (uint32_t i = 0; i < polygon.pointCount; i++) {
                    hashVector(hash, shape.points[polygon.firstPoint + i]);
                }
            }
        }
    }

    uint64_t contentHash(const Level& level)
    {
        derivedcache::Hash hash;
        hashShape(hash, level.model);
        hash.add(static_cast<uint64_t>(level.modelSurfaces));
        hash.add(static_cast<uint64_t>(level.skyZones.size()));
        for (bool sky : level.skyZones) {
            hash.add(sky);
        }
        hash.add(static_cast<uint64_t>(level.movers.size()));
        for (const Mover& mover : level.movers) {
            hash.add(mover.transform.actor);
            hashShape(hash, *mover.brush);
        }
        return hash.value;
    }

    Level describe(ULevel* level)
    {
        Level described;
        UModel* model = level ? level->model : nullptr;
        if (!model) {
            return described;
        }
        for (const MoverTransform& transform : moverTransforms(level)) {
            UObject* actor = level->actors[static_cast<size_t>(transform.actor)];
            objects::ensureLoaded(actor::Brush(actor));
            UModel* brush = cast<UModel>(actor::Brush(actor));
            described.movers.push_back({ transform, std::make_shared<const Shape>(brushShape(brush)) });
        }
        described.modelSurfaces = model->surfs.size();
        described.skyZones.assign(model->zones.size(), false);
        UClass* skyClass = engineClasses().skyZoneInfo;
        for (size_t i = 0; i < model->zones.size(); i++) {
            UObject* zone = model->zones[i].zoneActor;
            described.skyZones[i] = skyClass && zone && zone->data && zone->isA(skyClass);
        }
        // Each BSP surface once, the first time a node draws it; the nodes are the surface cut
        // into convex pieces.
        Shape& shape = described.model;
        std::vector<int32_t> surfaceIndex(model->surfs.size(), -1);
        for (size_t n = 0; n < model->nodes.size(); n++) {
            const BspNode& node = model->nodes[n];
            if (node.numVertices < 3 || node.surf < 0 || static_cast<size_t>(node.surf) >= model->surfs.size()) {
                continue;
            }
            int32_t& index = surfaceIndex[static_cast<size_t>(node.surf)];
            if (index < 0) {
                const BspSurf& surf = model->surfs[static_cast<size_t>(node.surf)];
                Surface surface = describeSurface(surf.texture, surf.polyFlags);
                surface.source = node.surf;
                shape.surfaces.push_back(std::move(surface));
                index = static_cast<int32_t>(shape.surfaces.size() - 1);
            }
            Polygon polygon;
            polygon.surface = index;
            polygon.node = static_cast<int32_t>(n);
            polygon.skyZone = (node.zone[0] < described.skyZones.size() && described.skyZones[node.zone[0]]) || (node.zone[1] < described.skyZones.size() && described.skyZones[node.zone[1]]);
            polygon.normal = { node.plane.x, node.plane.y, node.plane.z };
            polygon.firstPoint = static_cast<uint32_t>(shape.points.size());
            bool inRange = true;
            for (int32_t v = 0; v < node.numVertices && inRange; v++) {
                size_t vert = static_cast<size_t>(node.vertPool + v);
                inRange = vert < model->verts.size() && static_cast<size_t>(model->verts[vert].vertex) < model->points.size();
            }
            if (inRange) {
                for (int32_t v = 0; v < node.numVertices; v++) {
                    shape.points.push_back(model->points[static_cast<size_t>(model->verts[static_cast<size_t>(node.vertPool + v)].vertex)]);
                }
                polygon.pointCount = static_cast<uint32_t>(node.numVertices);
            }
            shape.polygons.push_back(polygon);
        }
        return described;
    }
}

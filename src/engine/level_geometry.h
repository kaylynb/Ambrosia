#pragma once

#include "core/math.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dx {
    class ULevel;
    class UModel;
    class UObject;
}

// A read-only description of a level's geometry, for code that needs the level as surfaces and
// polygons rather than as a BSP: the static level from its model, and its movers' brushes and
// where they are now. It says what each surface is (texture, flags, material group, and whether
// it is sky, a portal or invisible) and leaves to each consumer what to make of that.
namespace dx::levelgeometry {
    // Flags a texture adds to its surfaces' own (bsp.h has the surface flags): drawn masked,
    // translucent or modulated.
    constexpr uint32_t PF_Masked = 0x00000002;
    constexpr uint32_t PF_Translucent = 0x00000004;
    constexpr uint32_t PF_Modulated = 0x00000040;

    struct Surface {
        // The BSP surface in the model's surfs, or the polygon in a brush's polys.
        int32_t source = -1;
        UObject* texture = nullptr;
        std::string textureName;
        // The texture's group: its outer's name, as TraceTexture gives it and the footstep
        // scripts read it.
        std::string group;
        // The surface's flags with its texture's PF_Masked, PF_Translucent and PF_Modulated.
        uint32_t flags = 0;

        // The sky and painted-on distance (PF_FakeBackdrop).
        bool backdrop() const;
        // A zone portal (PF_Portal).
        bool portal() const;
        // Never drawn (PF_Invisible): collision hulls, the hidden faces of sheet brushes.
        bool invisible() const;
    };

    // A convex polygon, its points in order round it.
    struct Polygon {
        int32_t surface = -1;
        // The BSP node it is, or -1 for a brush's polygon.
        int32_t node = -1;
        // Whether either side of it is in a SkyZoneInfo zone, the skybox.
        bool skyZone = false;
        Vector normal;
        // Its points in Shape::points; none for a BSP node whose vertices are out of range.
        uint32_t firstPoint = 0;
        uint32_t pointCount = 0;
    };

    // One triangle of a polygon, as the fan from its first point gives them, with the polygon's
    // surface.
    struct Triangle {
        std::array<Vector, 3> points;
        int32_t polygon = -1;
        int32_t surface = -1;
    };

    struct Shape {
        // Each surface some polygon is of, in the order first met.
        std::vector<Surface> surfaces;
        std::vector<Polygon> polygons;
        std::vector<Vector> points;

        // The polygons as triangle fans, in order.
        std::vector<Triangle> triangles() const;
    };

    // Where a mover's brush is: the same transform the renderer draws it with.
    struct MoverTransform {
        // The mover's index in the level's actors.
        int32_t actor = 0;
        Vector location;
        Rotator rotation;
        Vector scale { 1, 1, 1 };
        Vector prePivot;

        bool operator==(const MoverTransform& other) const;
        // A point of the brush in the world.
        Vector place(const Vector& local) const;
    };

    struct Mover {
        MoverTransform transform;
        // The brush's polygons in the brush's own space, before the transform.
        std::shared_ptr<const Shape> brush;
    };

    struct Level {
        // The static level: every BSP node with a surface, in node order.
        Shape model;
        // Surfaces in the model, drawn by some node or not.
        size_t modelSurfaces = 0;
        // Which of the model's zones are skyboxes, by zone number.
        std::vector<bool> skyZones;
        // The movers that have a brush, in actor order; a destroyed mover is left out.
        std::vector<Mover> movers;
    };

    // The level described, loading the movers' brushes and every drawn surface's texture as it
    // goes. Empty for a level with no model.
    Level describe(ULevel* level);
    // A brush's polygons, one surface each, in the brush's own space.
    Shape brushShape(UModel* brush);
    // Where the level's movers are now, as describe lists them.
    std::vector<MoverTransform> moverTransforms(ULevel* level);
    // A surface of the given texture (loaded here) and flags.
    Surface describeSurface(UObject* texture, uint32_t flags);

    // A hash of the level's content as described, the same on every machine, for keying data
    // derived from it (core/derived_cache.h): every surface's texture name, group and flags,
    // every polygon's surface, node, skybox side, normal and points, the model's surface count
    // and skybox zones, and each mover's actor index and brush, but not where the movers are.
    uint64_t contentHash(const Level& level);
}

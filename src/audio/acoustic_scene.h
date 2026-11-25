#pragma once

#include "core/math.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dx {
    class ULevel;
    class UModel;
    class UObject;
}

// The level as the raytraced sound mode hears it: the BSP's surfaces as triangles, each with an
// acoustic material, or left out when it is not something sound could meet. This holds the
// geometry in Unreal units and axes; acoustics.cc converts it for Steam Audio.
namespace dx::audio::acoustics {
    // Deus Ex measures 16 units to the foot: the binoculars and scope show range in feet as the
    // distance over 16, and DeusExPlayer's TraceLOS of 8000 is "500 feet in front".
    constexpr float UnitsPerFoot = 16.0f;
    constexpr float UnitsPerMetre = UnitsPerFoot * 3.2808399f;

    // Unreal's left-handed x, y, z (z up) to Steam Audio's right-handed x, y, z (y up), in metres:
    // swapping y and z changes the handedness, and Unreal's z becomes Steam Audio's up.
    inline std::array<float, 3> toSteam(const Vector& unreal)
    {
        return { unreal.x / UnitsPerMetre, unreal.z / UnitsPerMetre, unreal.y / UnitsPerMetre };
    }

    // Acoustic properties for Steam Audio's three bands, centred on 400 Hz, 2.5 kHz and 15 kHz.
    struct Material {
        std::string name;
        std::array<float, 3> absorption {};
        float scattering = 0;
        std::array<float, 3> transmission {};
    };

    // The table the code ships with: entry 0 is "generic", the fallback.
    const std::vector<Material>& defaultMaterials();
    // An entry's index in the default table by name, or -1.
    int32_t materialIndex(std::string_view name);
    // A table as the lines of the table in acoustic_scene.cc, for pasting tuned values back.
    std::string materialTableText(const std::vector<Material>& table);

    // What decides a surface's material: its texture's group (the package group, as the footstep
    // scripts read it), the texture's name, the surface flags with the texture's own, and
    // whether either side of it is a skybox zone.
    struct SurfaceInfo {
        std::string group;
        std::string texture;
        uint32_t flags = 0;
        bool skyZone = false;
    };

    enum class Exclusion : uint8_t {
        None,
        Backdrop,
        Portal,
        Invisible,
        SkyZone,
        Effect,
        Count,
    };

    const char* exclusionName(Exclusion exclusion);

    struct Resolution {
        // Index into the material table, or -1 when the surface is left out.
        int32_t material = -1;
        Exclusion exclusion = Exclusion::None;
        // Which rule decided, for the debug overlay.
        const char* rule = "";
    };

    Resolution resolve(const SurfaceInfo& surface);

    struct Geometry;

    // A mover's brush, in the brush's own space, and where its actor puts it: the same
    // transform the renderer draws it with.
    struct MoverShape {
        int32_t id = 0;
        std::shared_ptr<const Geometry> shape;
    };

    struct MoverPose {
        int32_t id = 0;
        Vector location;
        Rotator rotation;
        Vector scale { 1, 1, 1 };
        Vector prePivot;

        bool operator==(const MoverPose& other) const
        {
            return id == other.id && location.x == other.location.x && location.y == other.location.y && location.z == other.location.z && rotation == other.rotation && scale.x == other.scale.x && scale.y == other.scale.y && scale.z == other.scale.z && prePivot.x == other.prePivot.x && prePivot.y == other.prePivot.y && prePivot.z == other.prePivot.z;
        }
    };

    // A point of a mover's shape in the world.
    Vector placePoint(const MoverPose& pose, const Vector& local);
    // The pose as Steam Audio's 4x4 transform (row-major, column vectors) from the shape's
    // vertices as toSteam gives them to the world as toSteam gives it.
    std::array<std::array<float, 4>, 4> steamTransform(const MoverPose& pose);

    // The level's BSP planes, copied so that where a point is can be asked off the game thread:
    // the same walk as the engine's solid and zone tests.
    struct SpaceNode {
        std::array<float, 4> plane {};
        int32_t front = -1;
        int32_t back = -1;
        std::array<uint8_t, 2> zone {};
        bool csg = false;
    };

    struct Space {
        std::vector<SpaceNode> nodes;
        bool rootOutside = true;
        // Zones that are skyboxes, by zone number.
        std::array<bool, 64> skyZone {};

        // Whether point is in the level's open space and not in a skybox. With no BSP, all of
        // space is open.
        bool open(const Vector& point) const;
    };

    struct Geometry {
        std::vector<Vector> vertices;
        std::vector<std::array<int32_t, 3>> triangles;
        // For each triangle, its surface and that surface's material.
        std::vector<int32_t> triangleSurface;
        std::vector<int32_t> triangleMaterial;
        std::vector<SurfaceInfo> surfaces;
        std::vector<Resolution> resolutions;
        // Triangles by why they were left out; the Exclusion::None count is those kept.
        std::array<size_t, static_cast<size_t>(Exclusion::Count)> triangleCounts {};
        // Surfaces in the model, drawn by some BSP node or not.
        size_t modelSurfaces = 0;
        // The level's movers, which the scene holds as instances it moves.
        std::vector<MoverShape> movers;
        // Where the open space is, for placing baked probes.
        Space space;

        size_t surfaceIndex(const SurfaceInfo& info);
        // Adds a convex polygon as a fan of triangles facing the way of normal, unless its
        // surface is left out.
        void addPolygon(const std::vector<Vector>& points, const Vector& normal, size_t surface);
    };

    // The nearest triangle a ray from origin along direction (unit length) meets within
    // maxDistance, or -1, with the distance to it. Checks every triangle; for the debug overlay.
    int32_t pickTriangle(const Geometry& geometry, const Vector& origin, const Vector& direction, float maxDistance, float& distance);

    // The level's BSP, in world space, with its movers' shapes.
    Geometry levelGeometry(ULevel* level);
    // Where the level's movers are now; a destroyed mover is left out.
    std::vector<MoverPose> moverPoses(ULevel* level);
}

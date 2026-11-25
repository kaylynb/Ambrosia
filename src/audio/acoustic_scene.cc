#include "audio/acoustic_scene.h"

#include "engine/bsp.h"
#include "engine/level_geometry.h"
#include "engine/resources.h"
#include "core/text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dx::audio::acoustics {
    namespace {
        constexpr uint32_t PF_Translucent = 0x00000004;
        constexpr uint32_t PF_Modulated = 0x00000040;

        // The acoustic material table, the one place its values live. Absorption and
        // transmission are for 400 Hz, 2.5 kHz and 15 kHz. Absorption and scattering start from
        // Steam Audio's published presets (phonon.h); see the notes where they differ.
        //
        // Transmission does not. Steam Audio applies it as a gain per band, and a BSP wall
        // between two rooms is two surfaces: through both, Steam Audio gave each face's value to
        // the power 1.5 (measured; acoustics_test). With the presets' values (concrete 0.015,
        // 0.002, 0.001) a guard behind any wall is silent, where Galaxy's line check never took a
        // sound below a third, about 10 dB down. The solid materials here pass 0.25, 0.16 and
        // 0.10 a face, so a concrete wall costs about 18, 24 and 30 dB: what is behind one stays
        // faintly audible and plainly duller. A window is one sheet, and costs about 7, 10 and
        // 16 dB.
        const std::vector<Material> materials = {
            // The fallback for anything the rules below do not place.
            { "generic", { 0.10f, 0.20f, 0.30f }, 0.05f, { 0.300f, 0.200f, 0.120f } },
            // About half of every level. Deus Ex's Metal group is mostly wall and floor panels
            // over solid brushes, so it keeps Steam Audio's metal absorption: hard in the mids
            // and highs, a little give in the lows; panels pass a little more than masonry.
            { "metal", { 0.20f, 0.07f, 0.06f }, 0.05f, { 0.300f, 0.200f, 0.120f } },
            // Steam Audio's rock.
            { "stone", { 0.13f, 0.20f, 0.24f }, 0.05f, { 0.250f, 0.160f, 0.100f } },
            { "concrete", { 0.05f, 0.07f, 0.08f }, 0.05f, { 0.250f, 0.160f, 0.100f } },
            { "wood", { 0.11f, 0.07f, 0.06f }, 0.05f, { 0.350f, 0.220f, 0.140f } },
            { "brick", { 0.03f, 0.04f, 0.07f }, 0.05f, { 0.250f, 0.160f, 0.100f } },
            // Window panes: Glass group surfaces drawn translucent or named as windows.
            { "glass", { 0.06f, 0.03f, 0.02f }, 0.05f, { 0.450f, 0.300f, 0.150f } },
            // The rest of the Glass group: light fittings, screens and covers. Hard, and no
            // window to hear through.
            { "fixture", { 0.05f, 0.04f, 0.03f }, 0.10f, { 0.300f, 0.200f, 0.100f } },
            // Carpet, as Steam Audio's preset.
            { "textile", { 0.24f, 0.69f, 0.73f }, 0.05f, { 0.300f, 0.200f, 0.120f } },
            // The Paper group is mostly painted walls and drywall (ClenWhitPaint_A,
            // FiniWhitDrywa_A), not paper: plaster-hard, with a panel's give in the lows.
            { "paper", { 0.10f, 0.05f, 0.04f }, 0.05f, { 0.400f, 0.250f, 0.150f } },
            // Leaves and bushes: scatter much and stop little.
            { "foliage", { 0.20f, 0.40f, 0.50f }, 0.70f, { 0.700f, 0.500f, 0.300f } },
            // Dirt and grass ground, between Steam Audio's rock and gravel.
            { "earth", { 0.30f, 0.40f, 0.50f }, 0.30f, { 0.250f, 0.160f, 0.100f } },
            { "ceramic", { 0.01f, 0.02f, 0.02f }, 0.05f, { 0.250f, 0.160f, 0.100f } },
            { "tiles", { 0.01f, 0.02f, 0.02f }, 0.05f, { 0.250f, 0.160f, 0.100f } },
            // Stucco, as Steam Audio's plaster.
            { "plaster", { 0.12f, 0.06f, 0.04f }, 0.05f, { 0.300f, 0.200f, 0.120f } },
            // Water surfaces reflect almost everything.
            { "water", { 0.01f, 0.01f, 0.02f }, 0.05f, { 0.250f, 0.150f, 0.080f } },
            // Masked surfaces (fences, grilles, gratings): mostly holes.
            { "grate", { 0.05f, 0.05f, 0.05f }, 0.30f, { 0.800f, 0.700f, 0.600f } },
            // Suspended ceiling tiles, found in the Tiles group by name: the one absorbent
            // surface in most offices.
            { "ceiling", { 0.40f, 0.70f, 0.60f }, 0.10f, { 0.500f, 0.350f, 0.200f } },
        };

        bool contains(const std::string& text, std::initializer_list<const char*> words)
        {
            return std::any_of(words.begin(), words.end(), [&](const char* word) {
                return text.find(word) != std::string::npos;
            });
        }

        // Texture groups the footstep scripts name, and the common ones outside them that are
        // one material throughout.
        const char* groupMaterial(const std::string& group)
        {
            static const std::pair<const char*, const char*> table[] = {
                { "metal", "metal" },
                { "coretexmetal", "metal" },
                { "ladder", "metal" },
                { "signs", "metal" },
                { "hk_signs", "metal" },
                { "stone", "stone" },
                { "concrete", "concrete" },
                { "wood", "wood" },
                { "brick", "brick" },
                { "textile", "textile" },
                { "paper", "paper" },
                { "foliage", "foliage" },
                { "earth", "earth" },
                { "ceramic", "ceramic" },
                { "tiles", "tiles" },
                { "tile", "tiles" },
                { "stucco", "plaster" },
                { "water", "water" },
                { "liquid", "water" },
                { "userinterface", "fixture" },
            };
            for (const auto& [name, material] : table) {
                if (group == name) {
                    return material;
                }
            }
            return nullptr;
        }

        // For the groups that mix materials (Misc, CoreTexMisc, Wall_Objects, the per-location
        // packages such as HK_Helibase or MJ12_lab): the material the texture's name says.
        const char* nameMaterial(const std::string& texture)
        {
            // Light fittings: Lights_A, ol_light_02, HelibaseLght_A, un_mj12light_b and the like.
            if (contains(texture, { "light", "lght" })) {
                return "fixture";
            }
            if (contains(texture, { "screen", "cmptr", "monitor", "worldmap" })) {
                return "fixture";
            }
            if (contains(texture, { "carpet", "rug", "cloth", "fabric", "curtain", "couch", "matress", "mattress" })) {
                return "textile";
            }
            if (contains(texture, { "wood", "plank", "board" })) {
                return "wood";
            }
            if (contains(texture, { "brick" })) {
                return "brick";
            }
            if (contains(texture, { "conc", "cncrt", "cmnt", "cement" })) {
                return "concrete";
            }
            if (contains(texture, { "stone", "rock", "marbl", "granit", "cobble" })) {
                return "stone";
            }
            if (contains(texture, { "tile" })) {
                return "tiles";
            }
            if (contains(texture, { "dirt", "grass", "mud", "ground", "sand" })) {
                return "earth";
            }
            if (contains(texture, { "water" })) {
                return "water";
            }
            if (contains(texture, { "plaster", "stucco" })) {
                return "plaster";
            }
            if (contains(texture, { "metal", "metl", "steel", "iron", "pipe", "tube", "vent", "grate", "panel", "plate", "duct", "rust", "alarm", "cab" })) {
                return "metal";
            }
            return nullptr;
        }

        // Single textures whose group says the wrong thing, or nothing, found by listing the
        // most used textures of each group in fifteen maps across the game.
        const char* textureOverride(const std::string& texture)
        {
            static const std::pair<const char*, const char*> table[] = {
                // Bulletproof windows, drawn masked as well as see-through.
                { "bulltprftrans_a", "glass" },
                // Paris Underground's floor, in Misc beside its Uob_Concrete walls.
                { "uob_floor_a", "concrete" },
                { "uob_conrmfloor", "concrete" },
                // The command centre walls of Vandenberg and others (Wall_Objects, V_Com_Center).
                { "clencmandwall_a", "concrete" },
                { "clencmandwall_b", "concrete" },
                { "clencmandwall_c", "concrete" },
                { "clencmandwall_j", "concrete" },
                { "clencmandwall_k", "concrete" },
                // Filing cabinet sides (Misc).
                { "filecab_siding", "metal" },
            };
            for (const auto& [name, material] : table) {
                if (texture == name) {
                    return material;
                }
            }
            return nullptr;
        }

        Resolution decided(const char* material, const char* rule)
        {
            Resolution resolution;
            resolution.material = materialIndex(material);
            resolution.rule = rule;
            return resolution;
        }

        Resolution excluded(Exclusion exclusion, const char* rule)
        {
            Resolution resolution;
            resolution.exclusion = exclusion;
            resolution.rule = rule;
            return resolution;
        }
    }

    const std::vector<Material>& defaultMaterials()
    {
        return materials;
    }

    int32_t materialIndex(std::string_view name)
    {
        for (size_t i = 0; i < materials.size(); i++) {
            if (materials[i].name == name) {
                return static_cast<int32_t>(i);
            }
        }
        return -1;
    }

    std::string materialTableText(const std::vector<Material>& table)
    {
        std::string text;
        char line[256];
        for (const Material& material : table) {
            std::snprintf(line, sizeof(line), "{ \"%s\", { %.2ff, %.2ff, %.2ff }, %.2ff, { %.3ff, %.3ff, %.3ff } },\n", material.name.c_str(), static_cast<double>(material.absorption[0]), static_cast<double>(material.absorption[1]), static_cast<double>(material.absorption[2]), static_cast<double>(material.scattering), static_cast<double>(material.transmission[0]), static_cast<double>(material.transmission[1]), static_cast<double>(material.transmission[2]));
            text += line;
        }
        return text;
    }

    const char* exclusionName(Exclusion exclusion)
    {
        switch (exclusion) {
        case Exclusion::None:
            return "kept";
        case Exclusion::Backdrop:
            return "sky and backdrop";
        case Exclusion::Portal:
            return "zone portals";
        case Exclusion::Invisible:
            return "invisible";
        case Exclusion::SkyZone:
            return "skybox zones";
        case Exclusion::Effect:
            return "effects and overlays";
        case Exclusion::Count:
            break;
        }
        return "?";
    }

    Resolution resolve(const SurfaceInfo& surface)
    {
        // What sound could not meet: the sky and painted-on distance, zone portals, surfaces
        // the game never draws (collision hulls, the hidden faces of sheet brushes), the skybox
        // rooms, and effects drawn as sheets.
        if (surface.flags & bsp::PF_FakeBackdrop) {
            return excluded(Exclusion::Backdrop, "fake backdrop flag");
        }
        if (surface.flags & bsp::PF_Portal) {
            return excluded(Exclusion::Portal, "portal flag");
        }
        if (surface.skyZone) {
            return excluded(Exclusion::SkyZone, "in a SkyZoneInfo zone");
        }
        if (surface.flags & bsp::PF_Invisible) {
            return excluded(Exclusion::Invisible, "invisible flag");
        }
        std::string group = lowered(surface.group);
        std::string texture = lowered(surface.texture);
        if (group == "sky" || group == "background" || group == "horizon") {
            return excluded(Exclusion::Backdrop, "backdrop texture group");
        }
        if (group == "fire" || group == "smoke" || group == "laser" || group == "electricity") {
            return excluded(Exclusion::Effect, "effect texture group");
        }
        if (surface.flags & PF_Modulated) {
            return excluded(Exclusion::Effect, "modulated overlay");
        }
        // Editor marker textures (Marker, Marker_sky) are never meant to be seen.
        if (texture.rfind("marker", 0) == 0) {
            return excluded(Exclusion::Invisible, "marker texture");
        }
        bool translucent = (surface.flags & PF_Translucent) != 0;
        bool namedGlass = contains(texture, { "glass", "window", "wndow", "wind" });
        if (const char* material = textureOverride(texture)) {
            return decided(material, "texture override");
        }
        // The Glass group holds windows (see-through, or opaque frosted ones such as
        // 07WindOpacStrek and OldeWhitGlass_A) and light fittings and covers, which are not.
        if (group == "glass") {
            if (translucent) {
                return decided("glass", "Glass group, translucent");
            }
            return namedGlass ? decided("glass", "Glass group, named as a window") : decided("fixture", "Glass group, opaque");
        }
        if (surface.flags & bsp::PF_Masked) {
            return decided("grate", "masked texture");
        }
        if (group == "tiles" && contains(texture, { "ceiln", "ceilin" })) {
            return decided("ceiling", "Tiles group, named as a ceiling");
        }
        if (const char* material = groupMaterial(group)) {
            return decided(material, "texture group");
        }
        if (namedGlass) {
            return translucent ? decided("glass", "named as glass, translucent") : decided("fixture", "named as glass, opaque");
        }
        // A see-through sheet that is not glass is a light shaft, hologram or the like.
        if (translucent && (surface.flags & bsp::PF_NotSolid)) {
            return excluded(Exclusion::Effect, "translucent non-solid sheet");
        }
        if (const char* material = nameMaterial(texture)) {
            return decided(material, "texture name");
        }
        return decided("generic", "fallback");
    }

    size_t Geometry::surfaceIndex(const SurfaceInfo& info)
    {
        surfaces.push_back(info);
        resolutions.push_back(resolve(info));
        return surfaces.size() - 1;
    }

    void Geometry::addPolygon(const std::vector<Vector>& points, const Vector& normal, size_t surface)
    {
        if (points.size() < 3) {
            return;
        }
        const Resolution& resolution = resolutions[surface];
        if (resolution.material < 0) {
            triangleCounts[static_cast<size_t>(resolution.exclusion)] += points.size() - 2;
            return;
        }
        int32_t base = static_cast<int32_t>(vertices.size());
        vertices.insert(vertices.end(), points.begin(), points.end());
        for (size_t i = 1; i + 1 < points.size(); i++) {
            std::array<int32_t, 3> triangle = { base, base + static_cast<int32_t>(i), base + static_cast<int32_t>(i + 1) };
            // Steam Audio takes the counter-clockwise side as the front in its right-handed
            // space. Converting from Unreal's left-handed axes mirrors the triangle, so the front
            // the normal points to must wind clockwise here.
            Vector a = points[0];
            Vector b = points[i];
            Vector c = points[i + 1];
            if ((b - a).cross(c - a).dot(normal) > 0) {
                std::swap(triangle[1], triangle[2]);
            }
            triangles.push_back(triangle);
            triangleSurface.push_back(static_cast<int32_t>(surface));
            triangleMaterial.push_back(resolution.material);
        }
        triangleCounts[0] += points.size() - 2;
    }

    int32_t pickTriangle(const Geometry& geometry, const Vector& origin, const Vector& direction, float maxDistance, float& distance)
    {
        int32_t best = -1;
        distance = maxDistance;
        for (size_t t = 0; t < geometry.triangles.size(); t++) {
            const Vector& a = geometry.vertices[static_cast<size_t>(geometry.triangles[t][0])];
            const Vector& b = geometry.vertices[static_cast<size_t>(geometry.triangles[t][1])];
            const Vector& c = geometry.vertices[static_cast<size_t>(geometry.triangles[t][2])];
            // Moeller and Trumbore's test, from both sides.
            Vector edge1 = b - a;
            Vector edge2 = c - a;
            Vector p = direction.cross(edge2);
            float determinant = edge1.dot(p);
            if (std::fabs(determinant) < 1e-8f) {
                continue;
            }
            float inverse = 1.0f / determinant;
            Vector s = origin - a;
            float u = s.dot(p) * inverse;
            if (u < 0 || u > 1) {
                continue;
            }
            Vector q = s.cross(edge1);
            float v = direction.dot(q) * inverse;
            if (v < 0 || u + v > 1) {
                continue;
            }
            float hit = edge2.dot(q) * inverse;
            if (hit > 0 && hit < distance) {
                distance = hit;
                best = static_cast<int32_t>(t);
            }
        }
        return best;
    }

    namespace {
        SurfaceInfo surfaceInfoOf(const levelgeometry::Surface& surface, bool skyZone)
        {
            SurfaceInfo info;
            info.group = surface.group;
            info.texture = surface.textureName;
            info.flags = surface.flags;
            info.skyZone = skyZone;
            return info;
        }

        levelgeometry::MoverTransform transformOf(const MoverPose& pose)
        {
            return { pose.id, pose.location, pose.rotation, pose.scale, pose.prePivot };
        }

        // A brush's polygons, each its own surface, none in a skybox.
        Geometry shapeGeometry(const levelgeometry::Shape& shape)
        {
            Geometry geometry;
            geometry.modelSurfaces = shape.surfaces.size();
            std::vector<Vector> points;
            for (const levelgeometry::Polygon& polygon : shape.polygons) {
                size_t surface = geometry.surfaceIndex(surfaceInfoOf(shape.surfaces[static_cast<size_t>(polygon.surface)], false));
                points.assign(shape.points.begin() + polygon.firstPoint, shape.points.begin() + polygon.firstPoint + polygon.pointCount);
                geometry.addPolygon(points, polygon.normal, surface);
            }
            return geometry;
        }
    }

    Vector placePoint(const MoverPose& pose, const Vector& local)
    {
        return transformOf(pose).place(local);
    }

    std::array<std::array<float, 4>, 4> steamTransform(const MoverPose& pose)
    {
        Coords axes = rotationAxes(pose.rotation);
        std::array<std::array<float, 4>, 4> m {};
        // Steam Audio's x, y and z are Unreal's x, z and y: each column is where a Steam Audio
        // axis of the shape ends up, scaled and turned as the renderer does it.
        const Vector unrealAxis[3] = { { 1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } };
        for (size_t column = 0; column < 3; column++) {
            Vector turned = untransformVector(axes, unrealAxis[column] * pose.scale);
            m[0][column] = turned.x;
            m[1][column] = turned.z;
            m[2][column] = turned.y;
        }
        std::array<float, 3> origin = toSteam(pose.location - untransformVector(axes, pose.prePivot * pose.scale));
        m[0][3] = origin[0];
        m[1][3] = origin[1];
        m[2][3] = origin[2];
        m[3][3] = 1;
        return m;
    }

    std::vector<MoverPose> moverPoses(ULevel* level)
    {
        std::vector<MoverPose> poses;
        for (const levelgeometry::MoverTransform& transform : levelgeometry::moverTransforms(level)) {
            poses.push_back({ transform.actor, transform.location, transform.rotation, transform.scale, transform.prePivot });
        }
        return poses;
    }

    Geometry levelGeometry(ULevel* level)
    {
        Geometry geometry;
        UModel* model = level ? level->model : nullptr;
        if (!model) {
            return geometry;
        }
        levelgeometry::Level described = levelgeometry::describe(level);
        for (const levelgeometry::Mover& mover : described.movers) {
            auto shape = std::make_shared<Geometry>(shapeGeometry(*mover.brush));
            if (!shape->triangles.empty()) {
                geometry.movers.push_back({ mover.transform.actor, std::move(shape) });
            }
        }
        geometry.modelSurfaces = described.modelSurfaces;
        const std::vector<bool>& skyZone = described.skyZones;
        // Each BSP surface once, as the engine lists them, then every node drawing it.
        const levelgeometry::Shape& shape = described.model;
        std::vector<int32_t> surfaceIndex(shape.surfaces.size(), -1);
        std::vector<Vector> points;
        for (const levelgeometry::Polygon& polygon : shape.polygons) {
            int32_t& index = surfaceIndex[static_cast<size_t>(polygon.surface)];
            // A surface touching a skybox zone anywhere is the skybox's: the nodes met before
            // that keep the surface as it was.
            if (index < 0 || (polygon.skyZone && !geometry.surfaces[static_cast<size_t>(index)].skyZone)) {
                index = static_cast<int32_t>(geometry.surfaceIndex(surfaceInfoOf(shape.surfaces[static_cast<size_t>(polygon.surface)], polygon.skyZone)));
            }
            points.assign(shape.points.begin() + polygon.firstPoint, shape.points.begin() + polygon.firstPoint + polygon.pointCount);
            geometry.addPolygon(points, polygon.normal, static_cast<size_t>(index));
        }
        geometry.space.rootOutside = model->rootOutside != 0;
        geometry.space.nodes.reserve(model->nodes.size());
        for (const BspNode& node : model->nodes) {
            SpaceNode copy;
            copy.plane = { node.plane.x, node.plane.y, node.plane.z, node.plane.w };
            copy.front = node.front;
            copy.back = node.back;
            copy.zone = { node.zone[0], node.zone[1] };
            copy.csg = bsp::isCsg(node, 0);
            geometry.space.nodes.push_back(copy);
        }
        for (size_t i = 0; i < skyZone.size() && i < geometry.space.skyZone.size(); i++) {
            geometry.space.skyZone[i] = skyZone[i];
        }
        return geometry;
    }

    bool Space::open(const Vector& point) const
    {
        if (nodes.empty()) {
            return true;
        }
        bool outside = rootOutside;
        int32_t index = 0;
        uint8_t zone = 0;
        while (index >= 0 && static_cast<size_t>(index) < nodes.size()) {
            const SpaceNode& node = nodes[static_cast<size_t>(index)];
            bool front = node.plane[0] * point.x + node.plane[1] * point.y + node.plane[2] * point.z - node.plane[3] >= 0.0f;
            outside = front ? (outside || node.csg) : (outside && !node.csg);
            zone = node.zone[front ? 1 : 0];
            index = front ? node.front : node.back;
        }
        return outside && !skyZone[zone & 63];
    }
}

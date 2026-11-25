#include "audio/acoustic_cache.h"
#include "test_paths.h"
#include "audio/acoustic_scene.h"
#include "core/objects.h"
#include "deusex/startup.h"
#include "engine/bsp.h"
#include "engine/level_geometry.h"
#include "engine/resources.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using dx::audio::acoustics::Geometry;

namespace {
    // FNV-1a over everything the export gives: counts, vertices, triangles with their surfaces and materials, the surfaces and their rules, and the movers.
    struct Digest {
        uint64_t value = 14695981039346656037ull;

        void bytes(const void* data, size_t size)
        {
            const auto* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; i++) {
                value ^= p[i];
                value *= 1099511628211ull;
            }
        }

        template <typename T>
        void add(const T& v)
        {
            bytes(&v, sizeof(v));
        }

        void text(const std::string& s)
        {
            add(s.size());
            bytes(s.data(), s.size());
        }

        void geometry(const Geometry& g)
        {
            add(g.modelSurfaces);
            add(g.triangleCounts);
            add(g.vertices.size());
            for (const dx::Vector& v : g.vertices) {
                add(v.x);
                add(v.y);
                add(v.z);
            }
            add(g.triangles.size());
            for (size_t i = 0; i < g.triangles.size(); i++) {
                add(g.triangles[i]);
                add(g.triangleSurface[i]);
                add(g.triangleMaterial[i]);
            }
            add(g.surfaces.size());
            for (size_t i = 0; i < g.surfaces.size(); i++) {
                text(g.surfaces[i].group);
                text(g.surfaces[i].texture);
                add(g.surfaces[i].flags);
                add(g.surfaces[i].skyZone);
                add(g.resolutions[i].material);
                add(g.resolutions[i].exclusion);
                text(g.resolutions[i].rule);
            }
            for (const auto& mover : g.movers) {
                add(mover.id);
                geometry(*mover.shape);
            }
        }
    };

    uint64_t digest(const Geometry& geometry)
    {
        Digest d;
        d.geometry(geometry);
        return d.value;
    }

}

TEST_CASE("a shape's polygons fan into triangles and a mover's transform places its brush", "[level]")
{
    dx::levelgeometry::Shape shape;
    shape.surfaces.resize(2);
    shape.points = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 }, { 5, 5, 5 }, { 6, 5, 5 }, { 6, 6, 5 } };
    shape.polygons.push_back({ 0, 3, false, { 0, 0, 1 }, 0, 4 });
    shape.polygons.push_back({ 1, 7, true, { 0, 0, 1 }, 4, 3 });
    // A node whose vertices were out of range has no points and no triangles.
    shape.polygons.push_back({ 1, 8, false, { 0, 0, 1 }, 7, 0 });
    std::vector<dx::levelgeometry::Triangle> triangles = shape.triangles();
    REQUIRE(triangles.size() == 3);
    CHECK(triangles[0].polygon == 0);
    CHECK(triangles[1].points[2].y == 1.0f);
    CHECK(triangles[1].points[1].x == 1.0f);
    CHECK(triangles[2].surface == 1);
    CHECK(triangles[2].points[0].x == 5.0f);

    dx::levelgeometry::MoverTransform transform;
    transform.location = { 100, 0, 0 };
    transform.scale = { 2, 2, 2 };
    transform.prePivot = { 1, 0, 0 };
    dx::Vector placed = transform.place({ 2, 0, 0 });
    CHECK(placed.x == Catch::Approx(102.0f));
    dx::levelgeometry::Surface surface;
    surface.flags = dx::bsp::PF_FakeBackdrop | dx::bsp::PF_Invisible;
    CHECK(surface.backdrop());
    CHECK(surface.invisible());
    CHECK_FALSE(surface.portal());
}

namespace {
    struct Expected {
        const char* map;
        size_t triangles;
        size_t vertices;
        size_t surfaces;
        size_t movers;
        // digest() of the export as it was made from the BSP directly, before the engine's level
        // geometry (the two were checked equal field by field when it moved), and the geometry
        // part of the bake cache's key, which bakes already on disk hold.
        uint64_t digest;
        uint64_t cacheGeometry;
    };

    const Expected expected[] = {
        { "01_NYC_UNATCOHQ", 13703, 23799, 3570, 27, 0x6d91fc6de85da9d3ull, 0x77c6919f60bdee48ull },
        { "06_HongKong_WanChai_Garage", 5327, 9523, 1004, 3, 0xf8c44bd13cda0db6ull, 0xac8664b2fbffe755ull },
        { "11_Paris_Cathedral", 38791, 67869, 8268, 30, 0x88344c190252c2efull, 0xd796aa062bf7b869ull },
        { "15_Area51_Page", 59209, 102063, 11161, 74, 0x91d247ee617e02a7ull, 0x9e1b93619e9da951ull },
        { "00_Intro", 20414, 35114, 4573, 27, 0x602fb6cc0c667808ull, 0x0fa067971fc930f1ull },
    };
}

TEST_CASE("the raytraced mode's export through the engine's level geometry is as it was", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    for (const Expected& want : expected) {
        CAPTURE(want.map);
        dx::objects::unloadPackage(dx::Name(want.map));
        {
            dx::World world;
            REQUIRE(world.loadMap(want.map));
            Geometry geometry = dx::audio::acoustics::levelGeometry(world.level);
            CHECK(geometry.triangles.size() == want.triangles);
            CHECK(geometry.vertices.size() == want.vertices);
            CHECK(geometry.surfaces.size() == want.surfaces);
            CHECK(geometry.movers.size() == want.movers);
            CHECK(digest(geometry) == want.digest);
            CHECK(dx::audio::acoustics::cacheKey(geometry, dx::audio::acoustics::defaultMaterials(), {}).geometry == want.cacheGeometry);

            // The engine's description holds every fan the export counts, kept or left out, and
            // says which surfaces are sky, portals and invisible.
            dx::levelgeometry::Level described = dx::levelgeometry::describe(world.level);
            size_t counted = 0;
            for (size_t count : geometry.triangleCounts) {
                counted += count;
            }
            CHECK(described.model.triangles().size() == counted);
            CHECK(described.modelSurfaces == geometry.modelSurfaces);
            size_t backdrops = 0;
            for (const dx::levelgeometry::Surface& surface : described.model.surfaces) {
                REQUIRE(surface.source >= 0);
                CHECK((surface.backdrop() == ((surface.flags & dx::bsp::PF_FakeBackdrop) != 0)));
                if (surface.backdrop()) {
                    backdrops++;
                }
            }
            for (size_t i = 0; i < geometry.surfaces.size(); i++) {
                if (geometry.surfaces[i].flags & dx::bsp::PF_FakeBackdrop) {
                    CHECK(geometry.resolutions[i].material < 0);
                }
            }
            CHECK(described.movers.size() == dx::levelgeometry::moverTransforms(world.level).size());
            std::printf("%s: %zu triangles kept of %zu, %zu of %zu surfaces backdrop\n", want.map, geometry.triangles.size(), counted, backdrops, described.model.surfaces.size());
        }
        dx::objects::unloadPackage(dx::Name(want.map));
    }
}
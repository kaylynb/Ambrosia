#include "core/objects.h"
#include "test_paths.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/map_writer.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {
    dx::ULevel* loadEditorLevel(dx::Linker* linker)
    {
        int32_t ref = linker->findExport(dx::Name("MyLevel"), dx::Name("Level"), "*");
        dx::ULevel* level = dx::cast<dx::ULevel>(linker->object(ref));
        dx::objects::ensureLoaded(level);
        dx::objects::flushLoads();
        for (dx::UObject* a : level->actors) {
            dx::objects::ensureLoaded(a);
        }
        dx::objects::flushLoads();
        dx::objects::ensureLoaded(level->model);
        dx::objects::flushLoads();
        return level;
    }

    dx::UObject* findActor(dx::ULevel* level, const char* name)
    {
        for (dx::UObject* a : level->actors) {
            if (a && a->name == dx::Name(name)) {
                return a;
            }
        }
        return nullptr;
    }

    size_t liveActors(dx::ULevel* level)
    {
        size_t count = 0;
        for (dx::UObject* a : level->actors) {
            if (a) {
                count++;
            }
        }
        return count;
    }

    uint32_t exportFlags(dx::Linker* linker, const char* name)
    {
        int32_t ref = linker->findExport(dx::Name(name), dx::Name(), "*");
        if (ref <= 0) {
            return 0;
        }
        return linker->file().exportAt(ref).flags;
    }

    // What the saved copy should keep, taken from the map as loaded so the test holds for any
    // version of it.
    struct LevelShape {
        size_t actors = 0;
        size_t nodes = 0;
        size_t surfs = 0;
        size_t points = 0;
        size_t verts = 0;
        size_t lightBits = 0;
        size_t leaves = 0;
        size_t leafHulls = 0;
        size_t reachSpecs = 0;
    };

    LevelShape shapeOf(dx::ULevel* level)
    {
        LevelShape shape;
        shape.actors = liveActors(level);
        shape.nodes = level->model->nodes.size();
        shape.surfs = level->model->surfs.size();
        shape.points = level->model->points.size();
        shape.verts = level->model->verts.size();
        shape.lightBits = level->model->lightBits.size();
        shape.leaves = level->model->leaves.size();
        shape.leafHulls = level->model->leafHulls.size();
        shape.reachSpecs = level->reachSpecs.size();
        return shape;
    }

    constexpr uint32_t editorOnly = dx::RF_NotForClient | dx::RF_NotForServer;

    // The first actor of the class that the map marks as editor only.
    std::string editorOnlyActor(dx::Linker* linker, dx::ULevel* level, const char* className)
    {
        for (dx::UObject* a : level->actors) {
            if (a && a->cls->name == dx::Name(className) && (exportFlags(linker, a->name.c_str()) & editorOnly) == editorOnly) {
                return a->name.str();
            }
        }
        return {};
    }
}

TEST_CASE("a saved map leaves out a deleted brush's model and polys", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::TempDir folder("editor-map-brushless");
    dx::test::EditorMode editor;
    dx::test::PackageGuard island("01_NYC_UNATCOIsland");
    dx::test::PackageGuard savedPackage("BrushlessIsland");
    std::filesystem::path file = folder / "BrushlessIsland.dx";
    std::string victim;
    std::string victimModel;
    std::string victimPolys;
    std::string keptModel;
    std::string keptPolys;
    {
        dx::Linker* linker = dx::objects::loadPackage(dx::Name("01_NYC_UNATCOIsland"));
        REQUIRE(linker != nullptr);
        dx::ULevel* level = loadEditorLevel(linker);
        std::vector<dx::UObject*> brushes;
        for (size_t i = 2; i < level->actors.size(); i++) {
            dx::UObject* a = level->actors[i];
            dx::UModel* model = a && a->isA(dx::Name("Brush")) ? dx::cast<dx::UModel>(dx::actor::Brush(a)) : nullptr;
            if (model) {
                dx::objects::ensureLoaded(model);
                dx::objects::flushLoads();
                if (model->polys) {
                    brushes.push_back(a);
                }
            }
        }
        REQUIRE(brushes.size() >= 2);
        dx::UModel* model = dx::cast<dx::UModel>(dx::actor::Brush(brushes[0]));
        victim = brushes[0]->name.str();
        victimModel = model->name.str();
        victimPolys = model->polys->name.str();
        dx::UModel* other = dx::cast<dx::UModel>(dx::actor::Brush(brushes[1]));
        keptModel = other->name.str();
        keptPolys = other->polys->name.str();
        level->actors.erase(std::find(level->actors.begin(), level->actors.end(), brushes[0]));
        brushes[0]->destroyed = true;
        std::string error;
        REQUIRE(dx::mapwriter::saveMap(level, file, &error));
    }
    dx::Linker* saved = dx::objects::loadPackageFile(dx::Name("BrushlessIsland"), file, false);
    REQUIRE(saved != nullptr);
    CHECK(saved->findExport(dx::Name(victim), dx::Name(), "*") <= 0);
    CHECK(saved->findExport(dx::Name(victimModel), dx::Name("Model"), "*") <= 0);
    CHECK(saved->findExport(dx::Name(victimPolys), dx::Name("Polys"), "*") <= 0);
    CHECK(saved->findExport(dx::Name(keptModel), dx::Name("Model"), "*") > 0);
    CHECK(saved->findExport(dx::Name(keptPolys), dx::Name("Polys"), "*") > 0);
}

TEST_CASE("saved map reloads with the same level and a moved actor", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::TempDir folder("editor-map-edited");
    dx::test::EditorMode editor;
    dx::test::PackageGuard island("01_NYC_UNATCOIsland");
    dx::test::PackageGuard savedPackage("EditedIsland");
    std::filesystem::path file = folder / "EditedIsland.dx";
    LevelShape original;
    std::string brush;
    std::string camera;
    {
        dx::Linker* linker = dx::objects::loadPackage(dx::Name("01_NYC_UNATCOIsland"));
        REQUIRE(linker != nullptr);
        dx::ULevel* level = loadEditorLevel(linker);
        REQUIRE(level->model != nullptr);
        original = shapeOf(level);
        brush = editorOnlyActor(linker, level, "Brush");
        camera = editorOnlyActor(linker, level, "Camera");
        dx::UObject* paul = findActor(level, "PaulDenton0");
        REQUIRE(paul != nullptr);
        dx::actor::Location(paul) = { -2266.0f, 8294.0f, -256.0f };
        std::string error;
        REQUIRE(dx::mapwriter::saveMap(level, file, &error));
        CHECK(error.empty());
    }
    size_t mismatchesBefore = dx::objects::loadMismatches().size();
    dx::Linker* saved = dx::objects::loadPackageFile(dx::Name("EditedIsland"), file, false);
    REQUIRE(saved != nullptr);
    dx::ULevel* level = loadEditorLevel(saved);
    REQUIRE(level != nullptr);
    REQUIRE(level->model != nullptr);
    LevelShape reloaded = shapeOf(level);
    CHECK(level->actors.size() == original.actors);
    CHECK(reloaded.actors == original.actors);
    CHECK(reloaded.nodes == original.nodes);
    CHECK(reloaded.surfs == original.surfs);
    CHECK(reloaded.points == original.points);
    CHECK(reloaded.verts == original.verts);
    CHECK(reloaded.lightBits == original.lightBits);
    CHECK(reloaded.leaves == original.leaves);
    CHECK(reloaded.leafHulls == original.leafHulls);
    CHECK(reloaded.reachSpecs == original.reachSpecs);
    dx::UObject* paul = findActor(level, "PaulDenton0");
    REQUIRE(paul != nullptr);
    CHECK(paul->outermost()->name == dx::Name("EditedIsland"));
    CHECK(dx::actor::Location(paul).x == -2266.0f);
    CHECK(dx::actor::Location(paul).y == 8294.0f);
    CHECK(dx::actor::Location(paul).z == -256.0f);
    CHECK(level->actors[0]->cls->name == dx::Name("LevelInfo"));
    // Actors the map keeps out of the game stay that way, and others do not become so.
    REQUIRE_FALSE(brush.empty());
    CHECK((exportFlags(saved, brush.c_str()) & editorOnly) == editorOnly);
    if (!camera.empty()) {
        CHECK((exportFlags(saved, camera.c_str()) & editorOnly) == editorOnly);
    }
    CHECK((exportFlags(saved, "PaulDenton0") & editorOnly) == 0);
    for (dx::UObject* a : level->actors) {
        dx::objects::ensureLoaded(dx::actor::Brush(a));
    }
    dx::objects::flushLoads();
    const std::vector<std::string>& mismatches = dx::objects::loadMismatches();
    std::string first = mismatches.size() > mismatchesBefore ? mismatches[mismatchesBefore] : std::string();
    INFO("first mismatch: " << first);
    CHECK(mismatches.size() == mismatchesBefore);
}

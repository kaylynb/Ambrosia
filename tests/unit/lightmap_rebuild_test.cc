#include "core/objects.h"
#include "test_paths.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/csg.h"
#include "engine/lightmap_builder.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

namespace {
    template <typename T>
    T* make(const char* className, dx::UObject* outer)
    {
        dx::UObject* object = dx::objects::newObject(dx::csg::nativeClass(className), outer, dx::Name(), 0);
        object->loaded = true;
        return dx::cast<T>(object);
    }
}

TEST_CASE("rebuilding lighting twice leaves the light lists the same size and keeps the leaf lists", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UObject* package = dx::objects::packageObject(dx::Name("LightRebuildTwiceTest"));
    dx::ULevel* level = make<dx::ULevel>("Level", package);
    level->model = make<dx::UModel>("Model", package);
    dx::UObject* builder = make<dx::UObject>("Brush", package);
    dx::UModel* builderModel = make<dx::UModel>("Model", package);
    builderModel->polys = make<dx::UPolys>("Polys", package);
    dx::actor::Brush(builder) = builderModel;
    level->actors = { nullptr, builder };
    dx::csg::buildCube(*builderModel->polys, 1024.0f, 1024.0f, 512.0f, nullptr);
    dx::csg::addBrush(level, builder, dx::csg::CsgSubtract);
    dx::csg::buildCube(*builderModel->polys, 128.0f, 128.0f, 128.0f, nullptr);
    dx::actor::Location(builder) = { 0.0f, 0.0f, -150.0f };
    dx::UObject* mover = dx::csg::addMover(level, builder, nullptr);
    REQUIRE(mover != nullptr);
    dx::UObject* light = dx::objects::newObject(dx::objects::findClass("Engine.Light"), package, dx::Name(), 0);
    light->loaded = true;
    dx::actor::Location(light) = { 0.0f, 0.0f, 100.0f };
    dx::actor::LightType(light) = 1;
    dx::actor::LightRadius(light) = 64;
    dx::actor::bStatic(light) = 1;
    level->actors.push_back(light);
    dx::csg::rebuild(level);
    dx::lightbuild::rebuild(level);
    dx::UModel& model = *level->model;
    dx::UModel& brush = *dx::cast<dx::UModel>(dx::actor::Brush(mover));
    size_t lights = model.lights.size();
    size_t bits = model.lightBits.size();
    size_t brushLights = brush.lights.size();
    size_t brushBits = brush.lightBits.size();
    REQUIRE(brushLights > 0);
    dx::lightbuild::rebuild(level);
    CHECK(model.lights.size() == lights);
    CHECK(model.lightBits.size() == bits);
    CHECK(brush.lights.size() == brushLights);
    CHECK(brush.lightBits.size() == brushBits);
    bool permeated = false;
    for (const dx::ConvexLeaf& leaf : model.leaves) {
        if (leaf.permeating >= 0) {
            permeated = true;
            CHECK(model.lights[static_cast<size_t>(leaf.permeating)] == light);
        }
    }
    CHECK(permeated);
}

#include "core/objects.h"
#include "test_paths.h"
#include "deusex/startup.h"
#include "engine/csg.h"
#include "engine/light_effects.h"
#include "engine/lightmap_builder.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "render/lighting.h"
#include "render/lightmap.h"

#include <catch_amalgamated.hpp>

#include <cmath>

namespace {
    dx::UObject* makeLight(uint8_t type, uint8_t period, uint8_t phase)
    {
        dx::engine::initialize(dx::test::gameData());
        dx::UObject* light = dx::objects::newObject(dx::objects::findClass("Engine.Light"), dx::objects::packageObject(dx::Name("LightAnimTest")), dx::Name(), 0);
        light->loaded = true;
        dx::actor::LightType(light) = type;
        dx::actor::LightBrightness(light) = 128;
        dx::actor::LightHue(light) = 0;
        dx::actor::LightSaturation(light) = 255;
        dx::actor::LightPeriod(light) = period;
        dx::actor::LightPhase(light) = phase;
        dx::actor::LightRadius(light) = 3;
        return light;
    }

    float brightnessRatio(dx::UObject* light, float time)
    {
        uint8_t type = dx::actor::LightType(light);
        dx::Vector animated = dx::lightColor(light, time);
        dx::actor::LightType(light) = 1;
        dx::Vector steady = dx::lightColor(light, time);
        dx::actor::LightType(light) = type;
        return animated.x / steady.x;
    }

    dx::UObject* makeEffectLight(uint8_t effect)
    {
        dx::UObject* light = makeLight(1, 0, 0);
        dx::actor::LightEffect(light) = effect;
        return light;
    }

    template <typename T>
    T* make(const char* className, dx::UObject* outer)
    {
        dx::UObject* object = dx::objects::newObject(dx::csg::nativeClass(className), outer, dx::Name(), 0);
        object->loaded = true;
        return dx::cast<T>(object);
    }

    struct Room {
        dx::ULevel* level = nullptr;
        dx::UObject* light = nullptr;
    };

    Room makeLitRoom(const char* packageName)
    {
        dx::engine::initialize(dx::test::gameData());
        dx::UObject* package = dx::objects::packageObject(dx::Name(packageName));
        Room room;
        room.level = make<dx::ULevel>("Level", package);
        room.level->model = make<dx::UModel>("Model", package);
        dx::UObject* builder = make<dx::UObject>("Brush", package);
        dx::UModel* builderModel = make<dx::UModel>("Model", package);
        builderModel->polys = make<dx::UPolys>("Polys", package);
        dx::actor::Brush(builder) = builderModel;
        room.level->actors.push_back(nullptr);
        room.level->actors.push_back(builder);
        dx::csg::buildCube(*builderModel->polys, 512.0f, 512.0f, 256.0f, nullptr);
        dx::csg::addBrush(room.level, builder, dx::csg::CsgSubtract);
        room.light = dx::objects::newObject(dx::objects::findClass("Engine.Light"), package, dx::Name(), 0);
        room.light->loaded = true;
        dx::actor::Location(room.light) = { 0.0f, 0.0f, 0.0f };
        dx::actor::LightType(room.light) = 1;
        dx::actor::LightBrightness(room.light) = 64;
        dx::actor::LightRadius(room.light) = 32;
        dx::actor::LightPeriod(room.light) = 35;
        dx::actor::bStatic(room.light) = 1;
        room.level->actors.push_back(room.light);
        dx::csg::rebuild(room.level);
        dx::lightbuild::rebuild(room.level);
        return room;
    }

    const dx::BspSurf* floorSurf(const dx::UModel& model)
    {
        for (const dx::BspSurf& surf : model.surfs) {
            if (model.vectors[static_cast<size_t>(surf.vNormal)].z > 0.99f) {
                return &surf;
            }
        }
        return nullptr;
    }
}

TEST_CASE("pulse and subtle pulse follow the table sine of 35 T over period plus phase", "[data]")
{
    dx::UObject* pulse = makeLight(2, 35, 0);
    CHECK(brightnessRatio(pulse, 0.25f) == Catch::Approx(0.99f).margin(0.00001));
    CHECK(brightnessRatio(pulse, 0.75f) == Catch::Approx(0.21f).margin(0.00001));
    dx::actor::LightPhase(pulse) = 64;
    CHECK(brightnessRatio(pulse, 0.5f) == Catch::Approx(0.21f).margin(0.00001));
    dx::UObject* subtle = makeLight(7, 35, 0);
    CHECK(brightnessRatio(subtle, 0.25f) == Catch::Approx(0.99f).margin(0.00001));
    CHECK(brightnessRatio(subtle, 0.75f) == Catch::Approx(0.81f).margin(0.00001));
}

TEST_CASE("blink keeps the low bit of the phase counter at a large level time", "[data]")
{
    dx::UObject* blink = makeLight(3, 0, 0);
    float even = 200.0f;
    float odd = 200.0f + 1.0f / 65536.0f;
    REQUIRE(odd != even);
    CHECK(dx::lightColor(blink, even).x > 0.1f);
    CHECK(dx::lightColor(blink, odd).x == 0.0f);
    CHECK(dx::truncateToInt(3000000000.0) == -1294967296);
}

TEST_CASE("fast wave shifts the integer distance before applying the time phase", "[data]")
{
    dx::UObject* wave = makeEffectLight(6);
    dx::UObject* plain = makeEffectLight(0);
    dx::Vector location { 0, 0, 22 };
    dx::Vector point { 0, 0, 0 };
    dx::Vector up { 0, 0, 1 };
    CHECK(dx::sqrtApprox(484.0f) == 22.0f);
    float base = dx::lightIllumination(plain, location, point, up, 0);
    REQUIRE(base > 0.0f);
    CHECK(dx::lightIllumination(wave, location, point, up, 0) / base == Catch::Approx(0.9494409f).margin(0.00001));
    CHECK(dx::lightIllumination(wave, location, point, up, 1.0f) / base == Catch::Approx(0.8148050f).margin(0.00001));
}

TEST_CASE("auto panning advances 35 texels per second of zone speed and wraps at 1024 texels")
{
    CHECK(dx::autoPanTexels(2.0f, 1.5f) == 105.0f);
    CHECK(dx::autoPanTexels(100.0f, 2.0f) == 856.0f);
    CHECK(dx::autoPanTexels(1.0f, -1.0f) == 989.0f);
    CHECK(dx::smallWavyPanU(0.0f) == Catch::Approx(4.0f).margin(0.00001));
    CHECK(dx::smallWavyPanV(0.0f) == Catch::Approx(8.0f).margin(0.00001));
    CHECK(dx::smallWavyPanU(1.0f) == Catch::Approx(4.066664f).margin(0.00001));
    CHECK(dx::smallWavyPanV(1.0f) == Catch::Approx(7.305239f).margin(0.00001));
}

TEST_CASE("continuous randoms move at most a sixteenth per tick while bases are redrawn")
{
    dx::tickLightRandoms(10.0f);
    float bases[256];
    float randoms[256];
    for (int32_t i = 0; i < 256; i++) {
        bases[i] = dx::lightRandomBase(i);
        randoms[i] = dx::lightRandom(i);
    }
    dx::tickLightRandoms(10.0f + 1.0f / 35.0f + 0.001f);
    int32_t basesChanged = 0;
    int32_t randomsChanged = 0;
    float largestStep = 0;
    for (int32_t i = 0; i < 256; i++) {
        if (dx::lightRandomBase(i) != bases[i]) {
            basesChanged++;
        }
        if (dx::lightRandom(i) != randoms[i]) {
            randomsChanged++;
        }
        largestStep = std::max(largestStep, std::abs(dx::lightRandom(i) - randoms[i]));
    }
    CHECK(basesChanged > 250);
    CHECK(randomsChanged > 200);
    CHECK(largestStep <= 1.0f / 16.0f + 0.00001f);
}

TEST_CASE("flicker bases are redrawn every frame in Original timing and five times a second in Steady")
{
    auto basesChangedBy = [](float from, float to) {
        dx::tickLightRandoms(from);
        float bases[256];
        for (int32_t i = 0; i < 256; i++) {
            bases[i] = dx::lightRandomBase(i);
        }
        dx::tickLightRandoms(to);
        int32_t changed = 0;
        for (int32_t i = 0; i < 256; i++) {
            changed += dx::lightRandomBase(i) != bases[i];
        }
        return changed;
    };
    dx::FrameTiming saved = dx::frameTiming;
    dx::frameTiming = dx::FrameTiming::Original;
    CHECK(basesChangedBy(20.01f, 20.02f) > 250);
    dx::frameTiming = dx::FrameTiming::Steady;
    CHECK(basesChangedBy(20.01f, 20.02f) == 0);
    CHECK(basesChangedBy(20.02f, 20.19f) == 0);
    CHECK(basesChangedBy(20.19f, 20.21f) > 250);
    dx::frameTiming = saved;
}

TEST_CASE("flicker bases hold while the level is paused in Steady timing but not in Original")
{
    auto basesChangedBy = [](float from, float to) {
        dx::tickLightRandoms(from, true);
        float bases[256];
        for (int32_t i = 0; i < 256; i++) {
            bases[i] = dx::lightRandomBase(i);
        }
        dx::tickLightRandoms(to, true);
        int32_t changed = 0;
        for (int32_t i = 0; i < 256; i++) {
            changed += dx::lightRandomBase(i) != bases[i];
        }
        return changed;
    };
    dx::FrameTiming saved = dx::frameTiming;
    dx::frameTiming = dx::FrameTiming::Original;
    CHECK(basesChangedBy(40.01f, 40.02f) > 250);
    dx::frameTiming = dx::FrameTiming::Steady;
    CHECK(basesChangedBy(40.05f, 41.05f) == 0);
    // Unpaused, the next step draws again.
    dx::tickLightRandoms(41.25f, true);
    float held = dx::lightRandomBase(7);
    dx::tickLightRandoms(41.45f, false);
    CHECK(dx::lightRandomBase(7) != held);
    dx::frameTiming = saved;
}

TEST_CASE("strobe lights hold while the level is paused in Steady timing but not in Original", "[data]")
{
    dx::UObject* light = makeLight(5, 0, 0);
    dx::UObject* info = dx::objects::newObject(dx::objects::findClass("Engine.LevelInfo"), dx::objects::packageObject(dx::Name("LightAnimTest")), dx::Name(), 0);
    info->loaded = true;
    dx::actor::Level(light) = info;
    auto at = [&](float seconds, bool paused) {
        dx::levelinfo::TimeSeconds(info) = seconds;
        dx::levelinfo::Pauser(info) = paused ? "Player" : "";
        return dx::strobeDark(light, 0);
    };
    dx::FrameTiming saved = dx::frameTiming;
    dx::frameTiming = dx::FrameTiming::Original;
    bool first = at(50.01f, true);
    CHECK(at(50.02f, true) != first);
    dx::frameTiming = dx::FrameTiming::Steady;
    CHECK_FALSE(at(50.05f, false));
    CHECK_FALSE(at(50.25f, true));
    CHECK_FALSE(at(50.45f, true));
    CHECK(at(50.25f, false));
    CHECK(at(50.65f, true));
    CHECK(at(50.85f, true));
    dx::actor::Level(light) = nullptr;
    dx::frameTiming = saved;
}

TEST_CASE("strobe lights switch every call in Original timing and five times a second in Steady", "[data]")
{
    dx::UObject* light = makeLight(5, 0, 0);
    dx::FrameTiming saved = dx::frameTiming;
    dx::frameTiming = dx::FrameTiming::Original;
    bool first = dx::strobeDark(light, 30.01f);
    CHECK(dx::strobeDark(light, 30.02f) != first);
    CHECK(dx::strobeDark(light, 30.03f) == first);
    dx::frameTiming = dx::FrameTiming::Steady;
    CHECK_FALSE(dx::strobeDark(light, 30.01f));
    CHECK_FALSE(dx::strobeDark(light, 30.15f));
    CHECK(dx::strobeDark(light, 30.25f));
    CHECK(dx::strobeDark(light, 30.35f));
    CHECK_FALSE(dx::strobeDark(light, 30.45f));
    CHECK(brightnessRatio(light, 30.25f) == 0.0f);
    CHECK(brightnessRatio(light, 30.45f) == Catch::Approx(1.0f));
    dx::frameTiming = saved;
}

TEST_CASE("a pulsing static light relights its surface over time while a steady one stays baked", "[data]")
{
    Room room = makeLitRoom("LightAnimRoomTest");
    dx::UModel& model = *room.level->model;
    const dx::BspSurf* floor = floorSurf(model);
    REQUIRE(floor != nullptr);
    REQUIRE(floor->lightMap >= 0);
    dx::Vector ambient {};
    size_t texel = 8 * 16 + 8;

    dx::SurfaceLighting steady = dx::prepareSurfaceLighting(&model, *floor, ambient);
    CHECK(steady.lights.empty());
    dx::LightmapTexels steadyEarly = dx::computeLightmap(&model, *floor, ambient, 0.25f);
    dx::LightmapTexels steadyLate = dx::computeLightmap(&model, *floor, ambient, 0.75f);
    REQUIRE(steadyEarly.colors.size() > texel);
    CHECK(steadyEarly.colors[texel].x > 0.05f);
    CHECK(steadyEarly.colors[texel].x == steadyLate.colors[texel].x);

    dx::actor::LightType(room.light) = 2;
    dx::SurfaceLighting pulsing = dx::prepareSurfaceLighting(&model, *floor, ambient);
    CHECK(pulsing.lights.size() == 1);
    dx::LightmapTexels bright = dx::computeLightmap(&model, *floor, ambient, 0.25f);
    dx::LightmapTexels dim = dx::computeLightmap(&model, *floor, ambient, 0.75f);
    CHECK(bright.colors[texel].x == Catch::Approx(steadyEarly.colors[texel].x * 0.99f).margin(2.0f / 127.5f));
    CHECK(dim.colors[texel].x == Catch::Approx(steadyEarly.colors[texel].x * 0.21f).margin(2.0f / 127.5f));
    CHECK(dim.colors[texel].x < bright.colors[texel].x - 0.05f);
}

TEST_CASE("a trigger light that is off when the lightmaps are made lights its surface once turned on", "[data]")
{
    // The training map's dark room: a saved game has its TriggerLights at brightness 0, and the
    // light switch turns them up after the level's lightmaps are made.
    Room room = makeLitRoom("TriggerLightOffRoomTest");
    dx::UModel& model = *room.level->model;
    const dx::BspSurf* floor = floorSurf(model);
    REQUIRE(floor != nullptr);
    REQUIRE(floor->lightMap >= 0);
    dx::Vector ambient {};
    size_t texel = 8 * 16 + 8;

    dx::actor::bStatic(room.light) = 0;
    dx::actor::LightBrightness(room.light) = 0;
    dx::SurfaceLighting lighting = dx::prepareSurfaceLighting(&model, *floor, ambient);
    REQUIRE(lighting.lights.size() == 1);
    std::vector<uint8_t> bytes;
    dx::evaluateSurfaceLighting(lighting, &model, *floor, 0, bytes);
    REQUIRE(bytes.size() > texel * 3);
    CHECK(bytes[texel * 3] == 0);

    dx::actor::LightBrightness(room.light) = 64;
    dx::evaluateSurfaceLighting(lighting, &model, *floor, 0, bytes);
    CHECK(bytes[texel * 3] > 6);
}

#define SDL_MAIN_HANDLED

#include "core/objects.h"
#include "offscreen_gl.h"
#include "test_paths.h"
#include "deusex/startup.h"
#include "engine/props_engine.h"
#include "engine/world.h"
#include "render/render_device.h"
#include "render/scene_renderer.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <catch_amalgamated.hpp>
#include <glad/gl.h>

#include <vector>

namespace {
    constexpr int frameWidth = 640;
    constexpr int frameHeight = 480;

    std::vector<uint8_t> render(dx::SceneRenderer& renderer, dx::World& world, const dx::Vector& eye, const dx::Rotator& rotation)
    {
        renderer.setOptions(dx::RenderOptions {});
        renderer.setTime(1.0);
        renderer.beginFrame(frameWidth, frameHeight);
        renderer.drawScene(&world, world.level, world.actors, eye, rotation, 90.0f, world.player());
        renderer.endFrame();
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        renderer.readPixels(rgba, width, height);
        CHECK(glGetError() == GL_NO_ERROR);
        return rgba;
    }

    dx::UObject* findActor(dx::World& world, const char* name)
    {
        for (dx::UObject* a : world.actors) {
            if (a && !a->destroyed && a->name == dx::Name(name)) {
                return a;
            }
        }
        return nullptr;
    }

    // How far the light's sphere lies from the eye, as the scene ranks its dynamic lights.
    float reach(dx::UObject* light, const dx::Vector& eye)
    {
        return (dx::actor::Location(light) - eye).size() - (static_cast<float>(dx::actor::LightRadius(light)) + 1.0f) * 25.0f;
    }
}

TEST_CASE("a burning barrel lights the far wall of the Smuggler's basement while more dynamic lights are nearer the view than a draw holds", "[data]")
{
    // The original lights each surface with every dynamic light reaching it
    // (FDynamicLight::Filter, FLightManager::SetupForSurf), not the ones nearest the view.
    dx::test::OffscreenContext gl("dynamic lights", frameWidth, frameHeight);
    if (!gl.context) {
        SKIP("no OpenGL 3.3 context");
    }
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("02_NYC_Smug"));
    dx::World world;
    REQUIRE(world.loadMap("02_NYC_Smug"));
    dx::UObject* barrel = findActor(world, "BarrelFire10");
    REQUIRE(barrel != nullptr);
    dx::Vector eye { -53, 14, 70 };
    dx::Rotator rotation { 0, 21400, 0 };

    // The barrel is not among the eight dynamic lights nearest the view the scene used to keep.
    int nearer = 0;
    for (dx::UObject* a : world.actors) {
        if (!a || a == barrel || a->destroyed || !a->data || !a->isA(dx::Name("Actor"))) {
            continue;
        }
        bool dynamic = dx::actor::bDynamicLight(a) || !(dx::actor::bStatic(a) || dx::actor::bNoDelete(a));
        if (dynamic && dx::actor::LightType(a) != 0 && dx::actor::LightBrightness(a) != 0 && dx::actor::LightRadius(a) != 0 && reach(a, eye) < reach(barrel, eye)) {
            nearer++;
        }
    }
    REQUIRE(nearer >= 8);

    dx::SceneRenderer renderer;
    render(renderer, world, eye, rotation);
    std::vector<uint8_t> lit = render(renderer, world, eye, rotation);
    dx::actor::LightType(barrel) = 0;
    std::vector<uint8_t> dark = render(renderer, world, eye, rotation);
    REQUIRE(lit.size() == dark.size());

    int brighter = 0;
    for (size_t pixel = 0; pixel < lit.size() / 4; pixel++) {
        int difference = (lit[pixel * 4] + lit[pixel * 4 + 1] + lit[pixel * 4 + 2]) - (dark[pixel * 4] + dark[pixel * 4 + 1] + dark[pixel * 4 + 2]);
        if (difference > 15) {
            brighter++;
        }
    }
    INFO("nearer barrels " << nearer << ", pixels the barrel brightens " << brighter);
    CHECK(brighter > 1000);
    dx::objects::unloadPackage(dx::Name("02_NYC_Smug"));
}

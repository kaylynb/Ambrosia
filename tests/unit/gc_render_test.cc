#define SDL_MAIN_HANDLED

#include "core/gc.h"
#include "test_paths.h"
#include "core/objects.h"
#include "deusex/startup.h"
#include "engine/mesh.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "render/scene_renderer.h"
#include "render/texture_cache.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <catch_amalgamated.hpp>
#include <glad/gl.h>

#include <vector>

namespace {
    constexpr int frameWidth = 320;
    constexpr int frameHeight = 240;

    struct OffscreenContext {
        SDL_Window* window = nullptr;
        SDL_GLContext context = nullptr;

        OffscreenContext()
        {
            SDL_SetMainReady();
            if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
                return;
            }
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
            window = SDL_CreateWindow("gc render", frameWidth, frameHeight, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
            if (!window) {
                return;
            }
            context = SDL_GL_CreateContext(window);
            if (context && !gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
                SDL_GL_DestroyContext(context);
                context = nullptr;
            }
        }

        ~OffscreenContext()
        {
            if (context) {
                SDL_GL_DestroyContext(context);
            }
            if (window) {
                SDL_DestroyWindow(window);
            }
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }

        OffscreenContext(const OffscreenContext&) = delete;
        OffscreenContext& operator=(const OffscreenContext&) = delete;
    };

    // A texture of the transient package with the pixels of a loaded one, so that it can be
    // collected without touching any package.
    dx::UTexture* transientCopy(dx::UTexture* source)
    {
        dx::UObject* object = dx::objects::newObject(dx::objects::findClass("Engine.Texture"), dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
        dx::UTexture* texture = dx::cast<dx::UTexture>(object);
        REQUIRE(texture != nullptr);
        texture->mips = source->mips;
        dx::bitmap::Format(texture) = dx::bitmap::Format(source);
        dx::bitmap::Palette(texture) = dx::bitmap::Palette(source);
        dx::bitmap::UBits(texture) = dx::bitmap::UBits(source);
        dx::bitmap::VBits(texture) = dx::bitmap::VBits(source);
        dx::bitmap::USize(texture) = dx::bitmap::USize(source);
        dx::bitmap::VSize(texture) = dx::bitmap::VSize(source);
        REQUIRE(texture->decodable());
        return texture;
    }

    void render(dx::SceneRenderer& renderer, dx::World& world, const dx::Vector& eye, const dx::Rotator& rotation)
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
    }

    // Destroys an actor and deletes it at once, as the original's CleanupDestroyed does.
    void destroyAndRelease(dx::World& world, dx::UObject* actor)
    {
        world.destroy(actor);
        dx::gc::release({ actor });
    }
}

TEST_CASE("the texture cache forgets a texture that is collected", "[data][gc]")
{
    OffscreenContext gl;
    REQUIRE(gl.context != nullptr);
    dx::engine::initialize(dx::test::gameData());
    dx::UTexture* pink = dx::cast<dx::UTexture>(dx::objects::loadObject("DeusExItems.Skins.PinkMaskTex", nullptr));
    REQUIRE(pink != nullptr);
    dx::objects::ensureLoaded(pink);
    dx::UTexture* texture = transientCopy(pink);
    dx::UTexture* kept = transientCopy(pink);
    {
        dx::TextureCache cache;
        cache.beginFrame(1.0);
        REQUIRE(cache.get(texture) != nullptr);
        REQUIRE(cache.getMasked(texture) != nullptr);
        REQUIRE(cache.get(kept) != nullptr);
        CHECK(cache.holds(texture));
        uint64_t generation = cache.generation();

        dx::gc::release({ texture });
        CHECK_FALSE(cache.holds(texture));
        CHECK(cache.holds(kept));
        CHECK(cache.generation() != generation);
        // The GL texture goes at the next frame.
        cache.beginFrame(2.0);
        CHECK(cache.get(kept) != nullptr);
        CHECK(glGetError() == GL_NO_ERROR);
    }
    dx::gc::release({ kept });
}

TEST_CASE("the scene renderer drops what it kept for a collected actor, mesh light and texture", "[data][gc]")
{
    OffscreenContext gl;
    REQUIRE(gl.context != nullptr);
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOHQ"));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        dx::UClass* crateClass = dx::objects::findClass("DeusEx.CrateUnbreakableSmall");
        dx::objects::flushLoads();
        REQUIRE(crateClass != nullptr);
        dx::UObject* trench = dx::objects::loadObject("DeusExCharacters.GM_Trench", nullptr);
        dx::UTexture* pink = dx::cast<dx::UTexture>(dx::objects::loadObject("DeusExItems.Skins.PinkMaskTex", nullptr));
        REQUIRE(trench != nullptr);
        REQUIRE(pink != nullptr);
        dx::objects::ensureLoaded(trench);
        dx::objects::ensureLoaded(pink);
        dx::UTexture* skin = transientCopy(pink);

        dx::Rotator view { 0, dx::actor::Rotation(player).yaw, 0 };
        dx::Vector forward = dx::rotationAxes(view).xAxis;
        dx::Vector front = dx::actor::Location(player) + forward * 60.0f;
        dx::Rotator facing { 0, (view.yaw + 32768) & 65535, 0 };
        dx::UObject* dummy = world.spawn(crateClass, nullptr, dx::Name(), front, facing, nullptr, true);
        REQUIRE(dummy != nullptr);
        dx::actor::Mesh(dummy) = trench;
        for (int i = 0; i < 8; i++) {
            dx::actor::MultiSkins(dummy, i) = skin;
        }
        // A moving light beside it, so the light caches and the mesh's lit state refer to it.
        dx::UObject* lamp = world.spawn(crateClass, nullptr, dx::Name(), front + dx::Vector { 0, 0, 40 }, facing, nullptr, true);
        REQUIRE(lamp != nullptr);
        dx::actor::bHidden(lamp) = 1;
        dx::actor::LightType(lamp) = 1;
        dx::actor::LightBrightness(lamp) = 255;
        dx::actor::LightRadius(lamp) = 16;
        dx::Vector eye = dx::actor::Location(player) + dx::Vector { 0, 0, 30 };

        dx::SceneRenderer renderer;
        render(renderer, world, eye, view);
        render(renderer, world, eye, view);
        REQUIRE(renderer.holds(dummy));
        REQUIRE(renderer.holds(skin));
        REQUIRE(renderer.holds(lamp));

        // The skin goes first, while the actor still wears it: the reference is set to None and
        // the mesh is drawn again with what remains.
        dx::gc::release({ skin });
        CHECK_FALSE(renderer.holds(skin));
        CHECK(dx::actor::MultiSkins(dummy, 0) == nullptr);
        CHECK(renderer.holds(dummy));
        render(renderer, world, eye, view);
        CHECK(renderer.holds(dummy));

        destroyAndRelease(world, lamp);
        CHECK_FALSE(renderer.holds(lamp));
        CHECK(renderer.holds(dummy));
        render(renderer, world, eye, view);
        CHECK_FALSE(renderer.holds(lamp));

        destroyAndRelease(world, dummy);
        CHECK_FALSE(renderer.holds(dummy));
        render(renderer, world, eye, view);
        CHECK_FALSE(renderer.holds(dummy));
        CHECK(renderer.cacheStats().meshDrawDead == 0);
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
}

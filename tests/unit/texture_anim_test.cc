#include "core/objects.h"
#include "test_paths.h"
#include "deusex/startup.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "render/texture_cache.h"

#include <catch_amalgamated.hpp>

namespace {
    dx::UTexture* makeTexture(dx::UClass* cls)
    {
        dx::UObject* object = dx::objects::newObject(cls, dx::objects::packageObject(dx::Name("Transient")), dx::Name(), 0);
        return dx::cast<dx::UTexture>(object);
    }
}

TEST_CASE("animated textures advance like UTexture::Update and Tick", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UClass* cls = dx::objects::findClass("Engine.Texture");
    REQUIRE(cls != nullptr);
    dx::UTexture* a = makeTexture(cls);
    dx::UTexture* b = makeTexture(cls);
    dx::UTexture* c = makeTexture(cls);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    dx::texture::AnimNext(a) = b;
    dx::texture::AnimNext(b) = c;
    dx::texture::AnimNext(c) = a;

    dx::texture::MaxFrameRate(a) = 0.0f;
    dx::TextureAnimation perFrame;
    CHECK(dx::TextureCache::advance(a, perFrame, 0.01) == b);
    CHECK(dx::TextureCache::advance(a, perFrame, 0.01) == b);
    CHECK(dx::TextureCache::advance(a, perFrame, 0.02) == c);
    CHECK(dx::TextureCache::advance(a, perFrame, 0.03) == a);

    dx::texture::MaxFrameRate(a) = 20.0f;
    dx::texture::MinFrameRate(a) = 10.0f;
    dx::TextureAnimation limited;
    limited.lastUpdate = 1.0;
    CHECK(dx::TextureCache::advance(a, limited, 1.03) == a);
    CHECK(dx::TextureCache::advance(a, limited, 1.06) == b);
    CHECK(limited.accumulator == 0.0f);
    CHECK(dx::TextureCache::advance(a, limited, 1.31) == c);
    CHECK(limited.accumulator == Catch::Approx(0.1f).margin(1e-5));

    dx::texture::MaxFrameRate(a) = 0.0f;
    dx::texture::PrimeCount(a) = 2;
    dx::TextureAnimation primed;
    CHECK(dx::TextureCache::advance(a, primed, 0.5) == a);
}

TEST_CASE("with steady frame timing, animated textures with no frame rate limit step 60 times a second", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UClass* cls = dx::objects::findClass("Engine.Texture");
    REQUIRE(cls != nullptr);
    dx::UTexture* a = makeTexture(cls);
    dx::UTexture* b = makeTexture(cls);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    dx::texture::AnimNext(a) = b;
    dx::texture::AnimNext(b) = a;
    dx::texture::MaxFrameRate(a) = 0.0f;

    dx::TextureAnimation capped;
    capped.lastUpdate = 1.0;
    dx::UTexture* shown = a;
    int changes = 0;
    for (int i = 1; i <= 480; i++) {
        dx::UTexture* next = dx::TextureCache::advance(a, capped, 1.0 + i / 480.0, 60.0f);
        if (next != shown) {
            changes++;
        }
        shown = next;
    }
    CHECK(changes == 60);

    dx::TextureAnimation perFrame;
    perFrame.lastUpdate = 1.0;
    shown = a;
    changes = 0;
    for (int i = 1; i <= 480; i++) {
        dx::UTexture* next = dx::TextureCache::advance(a, perFrame, 1.0 + i / 480.0);
        if (next != shown) {
            changes++;
        }
        shown = next;
    }
    CHECK(changes == 480);
}

TEST_CASE("with steady frame timing, animated textures catch up after long frames", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UClass* cls = dx::objects::findClass("Engine.Texture");
    REQUIRE(cls != nullptr);
    dx::UTexture* frames[3] = { makeTexture(cls), makeTexture(cls), makeTexture(cls) };
    for (int i = 0; i < 3; i++) {
        REQUIRE(frames[i] != nullptr);
    }
    for (int i = 0; i < 3; i++) {
        dx::texture::AnimNext(frames[i]) = frames[(i + 1) % 3];
        dx::texture::MaxFrameRate(frames[i]) = 0.0f;
    }
    // 64 steps a second, exact in binary. Over one second that is 64 steps, so the
    // sequence of three ends one frame on, however often it is updated.
    auto run = [&](int framesPerSecond, int& stepsInFirstFrame) {
        dx::TextureAnimation animation;
        animation.lastUpdate = 1.0;
        dx::UTexture* shown = nullptr;
        for (int i = 1; i <= framesPerSecond; i++) {
            shown = dx::TextureCache::advance(frames[0], animation, 1.0 + static_cast<double>(i) / framesPerSecond, 64.0f);
            if (i == 1) {
                stepsInFirstFrame = shown == frames[0] ? 0 : (shown == frames[1] ? 1 : 2);
            }
        }
        return shown;
    };
    int first = -1;
    CHECK(run(16, first) == frames[1]);
    // The first 1/16 second frame takes 4 steps, one past a full turn of the sequence.
    CHECK(first == 1);
    CHECK(run(512, first) == frames[1]);
}

TEST_CASE("with steady frame timing, animated textures with a frame rate of their own step at exactly that rate", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UClass* cls = dx::objects::findClass("Engine.Texture");
    REQUIRE(cls != nullptr);
    dx::UTexture* frames[3] = { makeTexture(cls), makeTexture(cls), makeTexture(cls) };
    for (int i = 0; i < 3; i++) {
        REQUIRE(frames[i] != nullptr);
    }
    for (int i = 0; i < 3; i++) {
        dx::texture::AnimNext(frames[i]) = frames[(i + 1) % 3];
    }
    // The head's rate is the one used: 32 a second, exact in binary, so one second is 32
    // steps, ending two frames on. At 8 fps that takes 4 steps a frame, which the original's
    // one step a frame could not do.
    dx::texture::MaxFrameRate(frames[0]) = 32.0f;
    dx::texture::MinFrameRate(frames[0]) = 0.0f;
    auto run = [&](int framesPerSecond) {
        dx::TextureAnimation animation;
        animation.lastUpdate = 1.0;
        dx::UTexture* shown = nullptr;
        for (int i = 1; i <= framesPerSecond; i++) {
            shown = dx::TextureCache::advance(frames[0], animation, 1.0 + static_cast<double>(i) / framesPerSecond, 60.0f);
        }
        return shown;
    };
    CHECK(run(8) == frames[2]);
    CHECK(run(128) == frames[2]);
    CHECK(run(1024) == frames[2]);
}

#pragma once

#include <cstdint>
#include <random>

namespace dx::random {
    // The original seeds the C runtime once at startup with srand(time(NULL)) and
    // every draw comes from rand(). Ambrosia keeps a separate stream for each part of
    // the game instead, and seed() starts all of them from one value, giving each a
    // different seed so they do not repeat each other.
    enum class Stream : uint8_t {
        Script,
        Engine,
        Pawn,
        Physics,
        Ai,
        Decal,
        Particles,
        ConSys,
        Lighting,
        FractalPaint,
        FireTable,
        CRuntime,
        Count,
    };

    // Reseeds every stream from value. The game, the editor and ucc call this once
    // at startup with clockSeed(); the unit tests call it with a fixed value.
    void seed(uint64_t value);

    // A seed taken from the clock, different on every launch.
    uint64_t clockSeed();

    std::mt19937& engine(Stream stream);

    // The seed stream got from the last call to seed(), for generators that are
    // not a std::mt19937. They compare generation() with the one they saw last to
    // notice that seed() was called again and restart from seedOf().
    uint32_t seedOf(Stream stream);
    uint32_t generation();
}

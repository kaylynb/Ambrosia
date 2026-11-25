#include "engine/wall_adjust.h"

#include <catch_amalgamated.hpp>

#include <cmath>
#include <limits>

// The values are what the code gives today, including the edge cases at a zero dot product.

namespace {
    void requireVector(const dx::Vector& v, float x, float y, float z)
    {
        CHECK(v.x == x);
        CHECK(v.y == y);
        CHECK(v.z == z);
    }
}

TEST_CASE("twoWallAdjust runs along the crease of two walls that face apart and turns to the desired direction")
{
    dx::Vector delta { 2, 3, 4 };
    dx::twoWallAdjust({ 0, 0, -1 }, delta, { 1, 0, 0 }, { 0, 1, 0 }, 0.5f);
    requireVector(delta, 0, 0, -2);

    delta = { 2, 3, 4 };
    dx::twoWallAdjust({ 0, 0, 1 }, delta, { 1, 0, 0 }, { 0, 1, 0 }, 0.5f);
    requireVector(delta, 0, 0, 2);

    // A crease move at right angles to the desired direction is kept, not flipped.
    delta = { 2, 3, 4 };
    dx::twoWallAdjust({ 1, 0, 0 }, delta, { 1, 0, 0 }, { 0, 1, 0 }, 0.5f);
    requireVector(delta, 0, 0, 2);
}

TEST_CASE("twoWallAdjust stops dead between two walls facing each other")
{
    dx::Vector delta { 2, 3, 4 };
    dx::twoWallAdjust({ 0, 1, 0 }, delta, { 1, 0, 0 }, { -1, 0, 0 }, 0.25f);
    requireVector(delta, 0, 0, 0);
}

TEST_CASE("twoWallAdjust slides along the new wall and stops if the slide does not go forward")
{
    dx::Vector delta { 2, 3, 4 };
    dx::twoWallAdjust({ 0, 1, 0 }, delta, { 1, 0, 0 }, { 0.6f, 0.8f, 0 }, 0.5f);
    requireVector(delta, 0, 1.5f, 2);

    delta = { 2, 3, 4 };
    dx::twoWallAdjust({ -1, -1, 0 }, delta, { 1, 0, 0 }, { 0.6f, 0.8f, 0 }, 0.5f);
    requireVector(delta, 0, 0, 0);

    // A slide at right angles to the desired direction is dropped.
    delta = { 2, 0, 4 };
    dx::twoWallAdjust({ 0, 1, 0 }, delta, { 1, 0, 0 }, { 0.6f, 0.8f, 0 }, 0.5f);
    requireVector(delta, 0, 0, 0);
}

TEST_CASE("Vector::normal follows FVector::SafeNormal: zero for a tiny or NaN vector, double precision otherwise")
{
    requireVector(dx::Vector { 3, 4, 0 }.normal(), 0.6f, 0.8f, 0.0f);
    // Below a square sum of 1e-8 the result is zero; a NaN sum also counts as below.
    requireVector(dx::Vector { 0.00009f, 0, 0 }.normal(), 0.0f, 0.0f, 0.0f);
    CHECK(dx::Vector { 0.00011f, 0, 0 }.normal().x == 1.0f);
    requireVector(dx::Vector { std::numeric_limits<float>::quiet_NaN(), 1, 0 }.normal(), 0.0f, 0.0f, 0.0f);
    // The square sum is not rounded to float on the way: with it rounded, x comes out one
    // step lower.
    dx::Vector v { 0.1f, 0.2f, 0.3f };
    double x = static_cast<double>(v.x);
    double y = static_cast<double>(v.y);
    double z = static_cast<double>(v.z);
    double scale = 1.0 / std::sqrt(x * x + y * y + z * z);
    requireVector(v.normal(), static_cast<float>(x * scale), static_cast<float>(y * scale), static_cast<float>(z * scale));
}

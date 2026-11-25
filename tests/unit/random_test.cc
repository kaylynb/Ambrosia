#include "core/random.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace {
    using dx::random::Stream;

    std::vector<uint32_t> draws(Stream stream)
    {
        std::vector<uint32_t> values;
        for (int i = 0; i < 8; i++) {
            values.push_back(static_cast<uint32_t>(dx::random::engine(stream)()));
        }
        return values;
    }

    std::vector<int> crtDraws()
    {
        std::vector<int> values;
        for (int i = 0; i < 8; i++) {
            values.push_back(std::rand());
        }
        return values;
    }
}

TEST_CASE("the same seed gives the same draws and another seed gives others")
{
    dx::random::seed(1);
    std::vector<uint32_t> first = draws(Stream::Script);
    std::vector<int> firstCrt = crtDraws();
    uint32_t firstFire = dx::random::seedOf(Stream::FireTable);

    dx::random::seed(2);
    CHECK(draws(Stream::Script) != first);
    CHECK(crtDraws() != firstCrt);
    CHECK(dx::random::seedOf(Stream::FireTable) != firstFire);

    dx::random::seed(1);
    CHECK(draws(Stream::Script) == first);
    CHECK(crtDraws() == firstCrt);
    CHECK(dx::random::seedOf(Stream::FireTable) == firstFire);

    dx::random::seed(1);
}

TEST_CASE("each stream starts from its own seed")
{
    dx::random::seed(1);
    std::vector<uint32_t> seen;
    for (size_t i = 0; i < static_cast<size_t>(Stream::Count); i++) {
        uint32_t value = dx::random::seedOf(static_cast<Stream>(i));
        CHECK(std::find(seen.begin(), seen.end(), value) == seen.end());
        seen.push_back(value);
    }
    CHECK(draws(Stream::Engine) != draws(Stream::Pawn));
    dx::random::seed(1);
}

TEST_CASE("seeding again tells the hand-written generators to restart")
{
    uint32_t before = dx::random::generation();
    dx::random::seed(1);
    CHECK(dx::random::generation() != before);
}

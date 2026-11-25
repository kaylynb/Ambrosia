#include "audio/extended.h"

#include <catch_amalgamated.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

// Expected values were produced by x87 long double arithmetic with x86-64 GCC

namespace {
    using dx::audio::Extended;

    std::pair<uint16_t, uint64_t> bits(const Extended& value)
    {
        uint16_t signExponent = 0;
        uint64_t fraction = 0;
        value.toX87(signExponent, fraction);
        return { signExponent, fraction };
    }

    std::pair<uint16_t, uint64_t> x87(uint16_t signExponent, uint64_t fraction)
    {
        return { signExponent, fraction };
    }

    Extended power(int exponent)
    {
        return Extended(std::ldexp(1.0, exponent));
    }
}

TEST_CASE("extended addition rounds half a unit to even and carries into the exponent")
{
    Extended one(1);
    CHECK(bits(one + power(-64)) == x87(0x3fff, 0x8000000000000000ull));
    CHECK(bits(one + Extended(3) * power(-64)) == x87(0x3fff, 0x8000000000000002ull));
    CHECK(bits((one + power(-63)) + power(-64)) == x87(0x3fff, 0x8000000000000002ull));
    CHECK(bits((Extended(2) - power(-63)) + power(-64)) == x87(0x4000, 0x8000000000000000ull));
}

TEST_CASE("extended subtraction rounds below one and cancels exactly")
{
    Extended one(1);
    CHECK(bits(one - power(-65)) == x87(0x3fff, 0x8000000000000000ull));
    CHECK(bits(one - power(-66)) == x87(0x3fff, 0x8000000000000000ull));
    CHECK(bits((one + power(-63)) - one) == x87(0x3fc0, 0x8000000000000000ull));
    CHECK(bits(one - one) == x87(0x0000, 0));
}

TEST_CASE("extended multiplication and division round their full width results")
{
    CHECK(bits(Extended(1) / Extended(3)) == x87(0x3ffd, 0xaaaaaaaaaaaaaaabull));
    CHECK(bits(Extended(1) / Extended(10)) == x87(0x3ffb, 0xcccccccccccccccdull));
    CHECK(bits(Extended(2) / Extended(3)) == x87(0x3ffe, 0xaaaaaaaaaaaaaaabull));
    CHECK(bits(Extended(4294967297ll) * Extended(4294967297ll)) == x87(0x403f, 0x8000000100000000ull));
    Extended belowTop = Extended(INT64_MAX);
    Extended aboveTop = Extended(static_cast<uint64_t>(INT64_MAX) + 2);
    CHECK(bits(belowTop * aboveTop) == x87(0x407d, 0x8000000000000000ull));
    // 1/255 as a double, scaled back up, lands just under one: Galaxy's master volume tops out at 126
    CHECK(bits(Extended(255) * Extended(1.0 / 255.0)) == x87(0x3ffe, 0xffffffffffffff00ull));
}

TEST_CASE("extended square roots are correctly rounded")
{
    CHECK(bits(sqrt(Extended(2))) == x87(0x3fff, 0xb504f333f9de6484ull));
    CHECK(bits(sqrt(Extended(16384) * Extended(1.0 / 32767.0))) == x87(0x3ffe, 0xb505a839fc9b8fbbull));
    CHECK(bits(sqrt(Extended(0))) == x87(0x0000, 0));
    Extended negative = sqrt(Extended(-1));
    CHECK_FALSE(negative == negative);
}

TEST_CASE("extended values round to float and double ties to even and truncate to integers")
{
    Extended one(1);
    CHECK(std::bit_cast<uint32_t>(static_cast<float>(one + power(-24))) == 0x3f800000u);
    CHECK(std::bit_cast<uint32_t>(static_cast<float>(one + power(-24) + power(-60))) == 0x3f800001u);
    CHECK(std::bit_cast<uint32_t>(static_cast<float>(one + Extended(3) * power(-24))) == 0x3f800002u);
    CHECK(std::bit_cast<uint64_t>(static_cast<double>(one + power(-53))) == 0x3ff0000000000000ull);
    CHECK(std::bit_cast<uint64_t>(static_cast<double>(one + power(-53) + power(-62))) == 0x3ff0000000000001ull);
    CHECK(static_cast<int64_t>(Extended(-2.75)) == -2);
    CHECK(static_cast<int64_t>(power(62) + Extended(0.5)) == 4611686018427387904ll);
    CHECK(static_cast<int32_t>(Extended(255) * Extended(1.0 / 255.0) * Extended(127)) == 126);
}

TEST_CASE("extended cosine stays within a few units of the FPU's")
{
    // fcos reduces with its own approximation of pi, so only near agreement is possible
    Extended pi = Extended::fromX87(0x4000, 0xc90fdaa22168c235ull);
    CHECK(bits(cos(Extended(0))) == x87(0x3fff, 0x8000000000000000ull));
    CHECK(static_cast<double>(cos(pi)) == -1.0);
    CHECK(std::abs(static_cast<double>(cos(Extended(1))) - 0.5403023058681398) < 1e-16);
    CHECK(std::abs(static_cast<double>(cos(Extended(6.0))) - 0.9601702866503660) < 1e-16);
}

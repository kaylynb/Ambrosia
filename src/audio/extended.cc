#include "audio/extended.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace dx::audio {
    namespace {
        struct U128 {
            uint64_t hi = 0;
            uint64_t lo = 0;
        };

        bool isZero(const U128& a)
        {
            return a.hi == 0 && a.lo == 0;
        }

        bool less(const U128& a, const U128& b)
        {
            return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
        }

        U128 add(const U128& a, const U128& b, bool& carry)
        {
            uint64_t lo = a.lo + b.lo;
            uint64_t hi = a.hi + b.hi;
            bool carryHi = hi < a.hi;
            uint64_t sum = hi + (lo < a.lo ? 1u : 0u);
            carry = carryHi || sum < hi;
            return { sum, lo };
        }

        U128 subtract(const U128& a, const U128& b)
        {
            return { a.hi - b.hi - (a.lo < b.lo ? 1u : 0u), a.lo - b.lo };
        }

        U128 shiftLeft(const U128& a, int n)
        {
            if (n == 0) {
                return a;
            }
            if (n >= 64) {
                return { a.lo << (n - 64), 0 };
            }
            return { (a.hi << n) | (a.lo >> (64 - n)), a.lo << n };
        }

        // Shifts right, folding every bit shifted out into the lowest bit so rounding still sees it
        U128 shiftRightSticky(const U128& a, int64_t n)
        {
            if (n <= 0) {
                return a;
            }
            if (n >= 128) {
                return { 0, isZero(a) ? 0u : 1u };
            }
            int shift = static_cast<int>(n);
            U128 result;
            uint64_t lost = 0;
            if (shift == 64) {
                result = { 0, a.hi };
                lost = a.lo;
            } else if (shift > 64) {
                result = { 0, a.hi >> (shift - 64) };
                lost = a.lo | (a.hi << (128 - shift));
            } else {
                result = { a.hi >> shift, (a.lo >> shift) | (a.hi << (64 - shift)) };
                lost = a.lo << (64 - shift);
            }
            result.lo |= lost != 0 ? 1u : 0u;
            return result;
        }

        int leadingZeros(const U128& a)
        {
            return a.hi != 0 ? std::countl_zero(a.hi) : 64 + std::countl_zero(a.lo);
        }

        U128 multiply(uint64_t a, uint64_t b)
        {
            uint64_t a0 = a & 0xffffffffu;
            uint64_t a1 = a >> 32;
            uint64_t b0 = b & 0xffffffffu;
            uint64_t b1 = b >> 32;
            uint64_t p00 = a0 * b0;
            uint64_t p01 = a0 * b1;
            uint64_t p10 = a1 * b0;
            uint64_t p11 = a1 * b1;
            uint64_t middle = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
            return { p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32), (middle << 32) | (p00 & 0xffffffffu) };
        }
    }

    struct ExtendedMath {
        using Kind = Extended::Kind;

        // The x87 exponent range; results below the smallest normal flush to zero instead of
        // going denormal, which only matters for magnitudes under 2^-16382
        static constexpr int32_t MaxExponent = 16383;
        static constexpr int32_t MinExponent = -16382;

        static Extended make(bool negative, int32_t exponent, uint64_t mantissa)
        {
            if (exponent > MaxExponent) {
                return Extended::special(Kind::Infinite, negative);
            }
            if (exponent < MinExponent) {
                return Extended::special(Kind::Zero, negative);
            }
            Extended result;
            result.kind = Kind::Finite;
            result.negative = negative;
            result.exponent = exponent;
            result.mantissa = mantissa;
            return result;
        }

        // Rounds value * 2^low to a 64-bit mantissa, to nearest with ties to even
        static Extended round(bool negative, int32_t low, U128 value)
        {
            int shift = leadingZeros(value);
            value = shiftLeft(value, shift);
            int32_t exponent = low + 127 - shift;
            uint64_t top = value.hi;
            bool half = (value.lo >> 63) != 0;
            bool rest = (value.lo << 1) != 0;
            if (half && (rest || (top & 1) != 0)) {
                top++;
                if (top == 0) {
                    top = 1ull << 63;
                    exponent++;
                }
            }
            return make(negative, exponent, top);
        }

        static Extended add(Extended a, Extended b)
        {
            if (a.kind == Kind::NaN || b.kind == Kind::NaN) {
                return Extended::special(Kind::NaN, false);
            }
            if (a.kind == Kind::Infinite) {
                return (b.kind == Kind::Infinite && a.negative != b.negative) ? Extended::special(Kind::NaN, true) : a;
            }
            if (b.kind == Kind::Infinite) {
                return b;
            }
            if (a.kind == Kind::Zero) {
                return b.kind == Kind::Zero ? Extended::special(Kind::Zero, a.negative && b.negative) : b;
            }
            if (b.kind == Kind::Zero) {
                return a;
            }
            if (a.exponent < b.exponent || (a.exponent == b.exponent && a.mantissa < b.mantissa)) {
                std::swap(a, b);
            }
            int32_t low = a.exponent - 127;
            U128 larger { a.mantissa, 0 };
            U128 smaller = shiftRightSticky({ b.mantissa, 0 }, static_cast<int64_t>(a.exponent) - b.exponent);
            if (a.negative == b.negative) {
                bool carry = false;
                U128 sum = dx::audio::add(larger, smaller, carry);
                if (carry) {
                    sum = shiftRightSticky(sum, 1);
                    sum.hi |= 1ull << 63;
                    low++;
                }
                return round(a.negative, low, sum);
            }
            U128 difference = subtract(larger, smaller);
            if (isZero(difference)) {
                return Extended::special(Kind::Zero, false);
            }
            return round(a.negative, low, difference);
        }

        static Extended multiply(const Extended& a, const Extended& b)
        {
            bool negative = a.negative != b.negative;
            if (a.kind == Kind::NaN || b.kind == Kind::NaN) {
                return Extended::special(Kind::NaN, false);
            }
            if (a.kind == Kind::Infinite || b.kind == Kind::Infinite) {
                bool zero = a.kind == Kind::Zero || b.kind == Kind::Zero;
                return zero ? Extended::special(Kind::NaN, true) : Extended::special(Kind::Infinite, negative);
            }
            if (a.kind == Kind::Zero || b.kind == Kind::Zero) {
                return Extended::special(Kind::Zero, negative);
            }
            return round(negative, a.exponent + b.exponent - 126, dx::audio::multiply(a.mantissa, b.mantissa));
        }

        static Extended divide(const Extended& a, const Extended& b)
        {
            bool negative = a.negative != b.negative;
            if (a.kind == Kind::NaN || b.kind == Kind::NaN) {
                return Extended::special(Kind::NaN, false);
            }
            if (a.kind == Kind::Infinite) {
                return b.kind == Kind::Infinite ? Extended::special(Kind::NaN, true) : Extended::special(Kind::Infinite, negative);
            }
            if (b.kind == Kind::Infinite) {
                return Extended::special(Kind::Zero, negative);
            }
            if (b.kind == Kind::Zero) {
                return a.kind == Kind::Zero ? Extended::special(Kind::NaN, true) : Extended::special(Kind::Infinite, negative);
            }
            if (a.kind == Kind::Zero) {
                return Extended::special(Kind::Zero, negative);
            }
            // Long division for 68 quotient bits, enough for 64 bits plus rounding; the running
            // remainder can reach 65 bits, which the high flag carries
            const int bits = 68;
            uint64_t remainder = a.mantissa;
            bool high = false;
            U128 quotient;
            for (int i = 0; i < bits; i++) {
                bool bit = high || remainder >= b.mantissa;
                if (bit) {
                    remainder -= b.mantissa;
                }
                quotient = shiftLeft(quotient, 1);
                quotient.lo |= bit ? 1u : 0u;
                high = (remainder >> 63) != 0;
                remainder <<= 1;
            }
            quotient.lo |= (high || remainder != 0) ? 1u : 0u;
            return round(negative, a.exponent - b.exponent - (bits - 1), quotient);
        }

        static Extended squareRoot(const Extended& a)
        {
            if (a.kind == Kind::NaN || (a.negative && a.kind != Kind::Zero)) {
                return Extended::special(Kind::NaN, true);
            }
            if (a.kind != Kind::Finite) {
                return a;
            }
            // Digit by digit over mantissa * 2^scale, with scale chosen so the exponent halves
            // exactly and the root comes out with 67 bits
            int32_t scale = ((a.exponent - 63 - 69) & 1) == 0 ? 69 : 70;
            int pairs = (64 + scale + 1) / 2;
            auto bitAt = [&](int index) -> uint64_t {
                int offset = index - scale;
                return (offset >= 0 && offset < 64) ? (a.mantissa >> offset) & 1 : 0;
            };
            U128 remainder;
            U128 root;
            for (int i = 0; i < pairs; i++) {
                int index = 2 * (pairs - i) - 1;
                remainder = shiftLeft(remainder, 2);
                remainder.lo |= (bitAt(index) << 1) | bitAt(index - 1);
                U128 trial = shiftLeft(root, 2);
                trial.lo |= 1;
                root = shiftLeft(root, 1);
                if (!less(remainder, trial)) {
                    remainder = subtract(remainder, trial);
                    root.lo |= 1;
                }
            }
            root.lo |= isZero(remainder) ? 0u : 1u;
            return round(false, (a.exponent - 63 - scale) / 2, root);
        }

        // Orders by magnitude, with zero below every finite value and infinity above
        static int compareMagnitude(const Extended& a, const Extended& b)
        {
            if (a.kind != b.kind) {
                auto rank = [](Kind kind) {
                    return kind == Kind::Zero ? 0 : kind == Kind::Finite ? 1
                                                                         : 2;
                };
                return rank(a.kind) < rank(b.kind) ? -1 : 1;
            }
            if (a.kind != Kind::Finite) {
                return 0;
            }
            if (a.exponent != b.exponent) {
                return a.exponent < b.exponent ? -1 : 1;
            }
            if (a.mantissa != b.mantissa) {
                return a.mantissa < b.mantissa ? -1 : 1;
            }
            return 0;
        }
    };

    Extended::Extended(double value)
    {
        uint64_t bits = std::bit_cast<uint64_t>(value);
        negative = (bits >> 63) != 0;
        int32_t field = static_cast<int32_t>((bits >> 52) & 0x7ff);
        uint64_t fraction = bits & ((1ull << 52) - 1);
        if (field == 0x7ff) {
            kind = fraction == 0 ? Kind::Infinite : Kind::NaN;
        } else if (field == 0) {
            if (fraction != 0) {
                int shift = std::countl_zero(fraction);
                kind = Kind::Finite;
                exponent = 63 - shift - 1074;
                mantissa = fraction << shift;
            }
        } else {
            kind = Kind::Finite;
            exponent = field - 1023;
            mantissa = ((1ull << 52) | fraction) << 11;
        }
    }

    Extended Extended::fromInteger(bool negative, uint64_t magnitude)
    {
        if (magnitude == 0) {
            return special(Kind::Zero, false);
        }
        int shift = std::countl_zero(magnitude);
        return ExtendedMath::make(negative, 63 - shift, magnitude << shift);
    }

    Extended Extended::special(Kind kind, bool negative)
    {
        Extended result;
        result.kind = kind;
        result.negative = negative;
        return result;
    }

    Extended Extended::fromX87(uint16_t signExponent, uint64_t fraction)
    {
        bool negative = (signExponent & 0x8000) != 0;
        int32_t field = signExponent & 0x7fff;
        if (field == 0x7fff) {
            return special((fraction << 1) == 0 ? Kind::Infinite : Kind::NaN, negative);
        }
        if (fraction == 0) {
            return special(Kind::Zero, negative);
        }
        int shift = std::countl_zero(fraction);
        return ExtendedMath::make(negative, std::max(field, 1) - 16383 - shift, fraction << shift);
    }

    void Extended::toX87(uint16_t& signExponent, uint64_t& fraction) const
    {
        uint16_t sign = negative ? 0x8000 : 0;
        if (kind == Kind::Finite) {
            signExponent = static_cast<uint16_t>(sign | (exponent + 16383));
            fraction = mantissa;
        } else if (kind == Kind::Zero) {
            signExponent = sign;
            fraction = 0;
        } else {
            signExponent = static_cast<uint16_t>(sign | 0x7fff);
            fraction = kind == Kind::Infinite ? 1ull << 63 : 3ull << 62;
        }
    }

    double Extended::toBinary(int precision, int minExponent, int maxExponent) const
    {
        if (kind == Kind::NaN) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (kind == Kind::Infinite) {
            return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        }
        if (kind == Kind::Zero) {
            return negative ? -0.0 : 0.0;
        }
        // Keep the bits from the leading one down to the lowest the target can store, which
        // stops at its smallest denormal
        int32_t low = std::max(exponent, minExponent) - (precision - 1);
        int64_t shift = static_cast<int64_t>(low) - (exponent - 63);
        uint64_t kept = 0;
        bool half = false;
        bool rest = false;
        if (shift == 64) {
            half = true;
            rest = (mantissa << 1) != 0;
        } else if (shift < 64) {
            kept = mantissa >> shift;
            half = ((mantissa >> (shift - 1)) & 1) != 0;
            rest = (mantissa & ((1ull << (shift - 1)) - 1)) != 0;
        }
        if (half && (rest || (kept & 1) != 0)) {
            kept++;
        }
        if (kept != 0 && low + (63 - std::countl_zero(kept)) > maxExponent) {
            return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        }
        double result = std::ldexp(static_cast<double>(kept), low);
        return negative ? -result : result;
    }

    Extended::operator double() const
    {
        return toBinary(53, -1022, 1023);
    }

    Extended::operator float() const
    {
        return static_cast<float>(toBinary(24, -126, 127));
    }

    int64_t Extended::truncate() const
    {
        if (kind == Kind::Zero || (kind == Kind::Finite && exponent < 0)) {
            return 0;
        }
        if (kind != Kind::Finite || exponent > 62) {
            return std::numeric_limits<int64_t>::min();
        }
        int64_t magnitude = static_cast<int64_t>(mantissa >> (63 - exponent));
        return negative ? -magnitude : magnitude;
    }

    Extended operator+(const Extended& a, const Extended& b)
    {
        return ExtendedMath::add(a, b);
    }

    Extended operator-(const Extended& a, const Extended& b)
    {
        return ExtendedMath::add(a, -b);
    }

    Extended operator*(const Extended& a, const Extended& b)
    {
        return ExtendedMath::multiply(a, b);
    }

    Extended operator/(const Extended& a, const Extended& b)
    {
        return ExtendedMath::divide(a, b);
    }

    Extended operator-(const Extended& a)
    {
        Extended result = a;
        result.negative = !a.negative;
        return result;
    }

    bool operator==(const Extended& a, const Extended& b)
    {
        if (a.kind == Extended::Kind::NaN || b.kind == Extended::Kind::NaN) {
            return false;
        }
        if (a.kind == Extended::Kind::Zero && b.kind == Extended::Kind::Zero) {
            return true;
        }
        return a.negative == b.negative && ExtendedMath::compareMagnitude(a, b) == 0;
    }

    bool operator<(const Extended& a, const Extended& b)
    {
        if (a.kind == Extended::Kind::NaN || b.kind == Extended::Kind::NaN || a == b) {
            return false;
        }
        bool aNegative = a.negative && a.kind != Extended::Kind::Zero;
        bool bNegative = b.negative && b.kind != Extended::Kind::Zero;
        if (aNegative != bNegative) {
            return aNegative;
        }
        int order = ExtendedMath::compareMagnitude(a, b);
        return aNegative ? order > 0 : order < 0;
    }

    Extended sqrt(const Extended& a)
    {
        return ExtendedMath::squareRoot(a);
    }

    // Not bit exact: the FPU's fcos and each C library's cosl round differently from one another.
    // This stays within a few units in the last place of them for the reverb cutoff's range.
    Extended cos(const Extended& a)
    {
        if (a.kind == Extended::Kind::NaN || a.kind == Extended::Kind::Infinite) {
            return Extended::special(Extended::Kind::NaN, true);
        }
        // Pi split into two 64-bit halves, so reducing an angle stays exact in the high half
        // and only the tiny low half rounds
        const Extended piHigh = Extended::fromX87(0x4000, 0xc90fdaa22168c234ull);
        const Extended piLow = Extended::fromX87(0x3fc0, 0xc4c6628b80dc1cd1ull);
        const Extended two(2);
        const Extended half(0.5);
        Extended x = a.negative ? -a : a;
        if (x > piHigh * Extended(4)) {
            return Extended(std::cos(static_cast<double>(a)));
        }
        if (x > two * piHigh) {
            x = (x - two * piHigh) - two * piLow;
        }
        if (x > piHigh) {
            x = (two * piHigh - x) + two * piLow;
        }
        bool flip = x > half * piHigh;
        if (flip) {
            x = (piHigh - x) + piLow;
        }
        // Near a quarter turn the cosine is small, where its series would cancel badly, so take
        // the sine of the remaining angle instead
        bool useSine = x > half * half * piHigh;
        if (useSine) {
            x = (half * piHigh - x) + half * piLow;
        }
        Extended square = x * x;
        Extended term = useSine ? x : Extended(1);
        Extended sum = term;
        for (int k = 1; k <= 20; k++) {
            int n = useSine ? 2 * k : 2 * k - 1;
            term = -(term * square / Extended(n * (n + 1)));
            sum = sum + term;
        }
        return flip ? -sum : sum;
    }

    const std::array<int16_t, 32768>& galaxyPanTable()
    {
        static const std::array<int16_t, 32768> table = [] {
            std::array<int16_t, 32768> values {};
            for (size_t i = 0; i < values.size(); i++) {
                values[i] = static_cast<int16_t>(sqrt(Extended(i) * Extended(1.0 / 32767.0)) * Extended(32767));
            }
            return values;
        }();
        return table;
    }
}

#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace dx::audio {
    // Galaxy did its level and panning math in x87 extended precision, but long double only has
    // that format with x86 GCC (MSVC and ARM use 64 bits, WebAssembly 128). This is the same format
    // in software, so every compiler produces the same results: a 64-bit mantissa, with + - * /
    // and sqrt rounded to nearest even exactly as the FPU does.
    class Extended {
    public:
        Extended() = default;
        explicit Extended(double value);

        explicit Extended(float value)
            : Extended(static_cast<double>(value))
        {
        }

        template <std::integral T>
        explicit Extended(T value)
        {
            if constexpr (std::is_signed_v<T>) {
                *this = fromInteger(value < 0, value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value));
            } else {
                *this = fromInteger(false, static_cast<uint64_t>(value));
            }
        }

        explicit operator double() const;
        explicit operator float() const;

        // Truncates toward zero like the FPU's integer store, giving the integer indefinite
        // value (the most negative one) when the result does not fit
        template <std::integral T>
        explicit operator T() const
        {
            int64_t value = truncate();
            if constexpr (std::is_signed_v<T> && sizeof(T) < sizeof(int64_t)) {
                if (value < std::numeric_limits<T>::min() || value > std::numeric_limits<T>::max()) {
                    return std::numeric_limits<T>::min();
                }
            }
            return static_cast<T>(value);
        }

        // The 80-bit memory layout: sign and biased exponent, then the explicit mantissa
        static Extended fromX87(uint16_t signExponent, uint64_t fraction);
        void toX87(uint16_t& signExponent, uint64_t& fraction) const;

        friend Extended operator+(const Extended& a, const Extended& b);
        friend Extended operator-(const Extended& a, const Extended& b);
        friend Extended operator*(const Extended& a, const Extended& b);
        friend Extended operator/(const Extended& a, const Extended& b);
        friend Extended operator-(const Extended& a);
        friend bool operator==(const Extended& a, const Extended& b);
        friend bool operator<(const Extended& a, const Extended& b);

        friend bool operator>(const Extended& a, const Extended& b)
        {
            return b < a;
        }

        friend bool operator<=(const Extended& a, const Extended& b)
        {
            return a < b || a == b;
        }

        friend bool operator>=(const Extended& a, const Extended& b)
        {
            return b < a || a == b;
        }

        friend Extended sqrt(const Extended& a);
        friend Extended cos(const Extended& a);

    private:
        enum class Kind : uint8_t {
            Zero,
            Finite,
            Infinite,
            NaN,
        };

        static Extended fromInteger(bool negative, uint64_t magnitude);
        static Extended special(Kind kind, bool negative);
        int64_t truncate() const;
        double toBinary(int precision, int minExponent, int maxExponent) const;

        // A finite value is mantissa * 2^(exponent - 63), with the mantissa's top bit set
        Kind kind = Kind::Zero;
        bool negative = false;
        int32_t exponent = 0;
        uint64_t mantissa = 0;

        friend struct ExtendedMath;
    };

    // Galaxy's pan law: entry i is sqrt(i / 32767) * 32767, worked out in extended precision as
    // the original does. The sound mixer and the music player both pan through it.
    const std::array<int16_t, 32768>& galaxyPanTable();
}

#include "core/random.h"

#include <array>
#include <chrono>
#include <cstdlib>

namespace dx::random {
    namespace {
        constexpr size_t StreamCount = static_cast<size_t>(Stream::Count);

        // Used until seed() is first called, so code that never seeds still gets
        // the same draws on every run.
        constexpr uint64_t DefaultSeed = 0;

        uint32_t derive(uint64_t value, size_t index)
        {
            // splitmix64, so nearby seeds and nearby streams give unrelated results.
            uint64_t z = value + 0x9e3779b97f4a7c15ull * static_cast<uint64_t>(index + 1);
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            z ^= z >> 31;
            return static_cast<uint32_t>(z ^ (z >> 32));
        }

        struct Streams {
            std::array<uint32_t, StreamCount> seeds {};
            std::array<std::mt19937, StreamCount> engines {};
            uint32_t generation = 0;

            explicit Streams(uint64_t value)
            {
                reseed(value);
            }

            void reseed(uint64_t value)
            {
                for (size_t i = 0; i < StreamCount; i++) {
                    seeds[i] = derive(value, i);
                    engines[i].seed(seeds[i]);
                }
                generation++;
            }
        };

        Streams& streams()
        {
            static Streams state(DefaultSeed);
            return state;
        }
    }

    void seed(uint64_t value)
    {
        Streams& state = streams();
        state.reseed(value);
        std::srand(state.seeds[static_cast<size_t>(Stream::CRuntime)]);
    }

    uint64_t clockSeed()
    {
        uint64_t wall = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        uint64_t steady = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return wall ^ (steady << 32) ^ (steady >> 32);
    }

    std::mt19937& engine(Stream stream)
    {
        return streams().engines[static_cast<size_t>(stream)];
    }

    uint32_t seedOf(Stream stream)
    {
        return streams().seeds[static_cast<size_t>(stream)];
    }

    uint32_t generation()
    {
        return streams().generation;
    }
}

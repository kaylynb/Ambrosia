#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// Valve's Steam Audio, loaded from phonon.dll or libphonon.so next to the program when an
// Ambrosia-only sound mode asks for it. Without the library, or in a build without
// AMBROSIA_STEAM_AUDIO, create() returns null and the mixer keeps Galaxy's stereo panning.
namespace dx::audio::steam {
    // Frames Steam Audio processes at a time. The mixer renders whole blocks of this size while a
    // Steam Audio mode is on.
    constexpr size_t BlockFrames = 512;

    // Whether the library loads and makes a context, trying it the first time.
    bool available();

    // A unit vector from the listener to the source in Steam Audio's listener space: +x right,
    // +y up, -z ahead.
    struct Direction {
        float x = 0;
        float y = 0;
        float z = -1;
    };

    // HRTF spatialisation of mono sources with the built-in HRTF, one effect per mixer channel.
    class Binaural {
    public:
        static std::unique_ptr<Binaural> create(uint32_t sampleRate, size_t effects);
        ~Binaural();
        Binaural(const Binaural&) = delete;
        Binaural& operator=(const Binaural&) = delete;

        // Spatialises BlockFrames of mono through the given effect into left and right. blend is
        // Steam Audio's spatial blend: 0 leaves the input unchanged in both ears, 1 is fully spatial.
        void apply(size_t effect, float* mono, float* left, float* right, Direction direction, float blend);
        // Writes BlockFrames of what the effect still rings with after its input ended; false once
        // nothing remains.
        bool tail(size_t effect, float* left, float* right);
        void reset(size_t effect);
        // The gain that brings the effect's output, averaged over directions in the horizontal
        // plane, to the power of the input, for the blend applied.
        static float loudness(float blend);

    private:
        struct Impl;
        explicit Binaural(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl;
    };
}

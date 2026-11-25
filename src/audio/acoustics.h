#pragma once

#include "audio/acoustic_bake.h"
#include "audio/acoustic_scene.h"
#include "audio/steam_audio.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Steam Audio's scene and simulator for the raytraced sound mode. One worker thread owns every
// scene and simulator call, so scene changes never meet a running simulation; the game thread
// only hands it inputs, and the mixer only reads its latest results, never waiting for them.
namespace dx::audio::acoustics {
    struct Stats {
        bool embree = false;
        bool scene = false;
        // Whether the worker is paused (the mode off, its scene kept), and its passes so far.
        bool paused = false;
        uint64_t passes = 0;
        // Triangles of the level and its movers, and how many movers.
        size_t triangles = 0;
        size_t movers = 0;
        size_t materials = 0;
        // The last scene build on the worker, in milliseconds.
        double buildMilliseconds = 0;
        // The worker's last pass: sources simulated and the milliseconds the direct simulation
        // took, and passes per second over the last second.
        size_t sources = 0;
        double directMilliseconds = 0;
        double passesPerSecond = 0;
        // The last reflection simulation around the listener: its milliseconds, rays and
        // bounces, and the decay times (RT60, seconds) it gave at 400 Hz, 2.5 kHz and 15 kHz.
        double reflectionMilliseconds = 0;
        int32_t rays = 0;
        int32_t bounces = 0;
        std::array<float, 3> reverbTimes {};
        // Baked data: whether a probe batch is in use and its probes, whether the last reverb
        // came from it (false: the live simulation, asked for or with no probe near the
        // listener), the milliseconds the last pathing run took and how many sources it found a
        // path for, and why a batch handed over could not be used, if it could not.
        bool baked = false;
        size_t probes = 0;
        bool reverbBaked = false;
        double pathMilliseconds = 0;
        size_t pathed = 0;
        std::string bakeError;
    };

    // What reaches the listener from a source by the baked paths around what is in the way: per
    // band, the amplitude left after bending round edges (1 for a source in plain view, whose
    // path is the straight line), how long the way is in Unreal units, and the way it arrives
    // from, in Unreal world axes from the listener. focus is 1 for sound from one way, falling
    // towards 0 as it arrives from several.
    struct PathResult {
        bool found = false;
        std::array<float, 3> amplitude {};
        float length = 0;
        Vector direction;
        float focus = 0;
        // Steam Audio's own outputs: the band EQ and the first-order sound field (world axes).
        std::array<float, 3> eq {};
        std::array<float, 4> field {};
    };

    // One leg of the path Steam Audio found for the traced source, in Unreal units.
    struct PathSegment {
        Vector from;
        Vector to;
        bool occluded = false;
    };

    // Which baked data the simulation uses when it has some.
    struct BakedUse {
        bool reverb = true;
        bool pathing = true;

        bool operator==(const BakedUse&) const = default;
    };

    // How the listener's reverb is simulated: rays traced from the listener and how many times
    // each reflects. The simulator allows up to MaxRays.
    constexpr int32_t MaxRays = 8192;

    struct ReflectionSettings {
        int32_t rays = 4096;
        int32_t bounces = 16;
        // Whether baked reverb is used where there is some (from BakedUse).
        bool baked = true;
    };

    // The listener-centred reverb the mixer runs its reverb send through in the raytraced mode:
    // Steam Audio's parametric reverb at the simulated decay times, one for each ear's send.
    class ListenerReverb {
    public:
        ~ListenerReverb();
        ListenerReverb(const ListenerReverb&) = delete;
        ListenerReverb& operator=(const ListenerReverb&) = delete;

        // Adds steam::BlockFrames of reverb of the interleaved stereo send to out, times level.
        void process(const float* send, float* out, const std::array<float, 3>& times, float level);
        void reset();

    private:
        friend class Simulation;
        struct Impl;
        explicit ListenerReverb(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl;
    };

    // What reaches the listener straight from a source: the fraction not occluded, and for the
    // occluded part, what passes through the surfaces in the way at 400 Hz, 2.5 kHz and 15 kHz.
    struct DirectPath {
        float occlusion = 1;
        std::array<float, 3> transmission { 1, 1, 1 };
    };

    struct SourceInput {
        int32_t id = 0;
        Vector location;
        // Galaxy's priority for the voice: which hidden sources are pathed first.
        float priority = 0;
    };

    // The direct-path filters the mixer runs each positioned voice through, one per channel.
    class DirectFilters {
    public:
        ~DirectFilters();
        DirectFilters(const DirectFilters&) = delete;
        DirectFilters& operator=(const DirectFilters&) = delete;

        // Filters steam::BlockFrames of mono in place.
        void apply(size_t channel, float* mono, const DirectPath& path);
        void reset(size_t channel);
        // Makes every filter afresh: Steam Audio's reset leaves a direct effect easing from its
        // last parameters for a block or two, which a mode switch should not hear.
        void renew();

    private:
        friend class Simulation;
        struct Impl;
        explicit DirectFilters(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl;
    };

    // The pathed sound's own filters and HRTF for each channel: a band gain for the amplitude
    // that comes round, then spatialised from the way it arrives.
    class PathFilters {
    public:
        ~PathFilters();
        PathFilters(const PathFilters&) = delete;
        PathFilters& operator=(const PathFilters&) = delete;

        // Filters steam::BlockFrames of mono in place by the band amplitudes (each 0 to 1), then
        // spatialises it into left and right.
        void apply(size_t channel, float* mono, const std::array<float, 3>& amplitude, float* left, float* right, steam::Direction direction, float blend);
        bool tail(size_t channel, float* left, float* right);
        void reset(size_t channel);
        void renew();

    private:
        friend class Simulation;
        struct Impl;
        explicit PathFilters(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl;
    };

    class Simulation {
    public:
        // Null when Steam Audio or its simulation functions are unavailable.
        static std::unique_ptr<Simulation> create(uint32_t sampleRate, size_t channels);
        ~Simulation();
        Simulation(const Simulation&) = delete;
        Simulation& operator=(const Simulation&) = delete;

        // The level to simulate in, or null for none. The worker builds the scene from it.
        void setGeometry(std::shared_ptr<const Geometry> geometry);
        std::shared_ptr<const Geometry> geometry() const;
        // The material table, in the default table's order; changes reach the scene live.
        void setMaterials(const std::vector<Material>& materials);
        // Where sound is heard from, and the sources to simulate, from the game thread.
        void setListener(const Vector& location, const Rotator& rotation);
        void setSources(std::vector<SourceInput> sources);
        // Where the level's movers are, whenever any has moved.
        void setMoverPoses(std::vector<MoverPose> poses);
        // Copies the latest direct paths into paths if they are newer than version, without
        // waiting: false when the worker is publishing or nothing is new. For the mixer.
        bool directPaths(std::unordered_map<int32_t, DirectPath>& paths, uint64_t& version) const;
        // The latest decay times around the listener, the same way; false leaves times as
        // they were.
        bool reverbTimes(std::array<float, 3>& times) const;
        void setReflectionSettings(const ReflectionSettings& settings);
        // Baked data for the level the simulation is in, or null for none; the worker loads it
        // between passes and from then on uses it as BakedUse says. Data that will not load is
        // dropped and the reason kept in Stats::bakeError; the live simulation carries on.
        void setBaked(std::shared_ptr<const BakedData> data);
        void setBakedUse(const BakedUse& use);
        // The latest paths, as directPaths gives the direct ones; empty while there is no baked
        // data or pathing is off.
        bool pathResults(std::unordered_map<int32_t, PathResult>& paths, uint64_t& version) const;
        // The source whose path legs are kept for the overlay, or 0 for none, and those legs.
        void setTracedSource(int32_t id);
        std::vector<PathSegment> tracedPath() const;
        // Whether this simulation can use baked data at all.
        bool canUseBaked() const;
        DirectFilters& filters();
        PathFilters& pathFilters();
        ListenerReverb& reverb();
        Stats stats() const;
        // Pauses the worker, keeping its scene and sources, or wakes it. Paused, it does no work
        // and its last results are dropped: from the call on, no pass starts, and one already
        // running commits no results and is not counted in Stats::passes. It does not wait for
        // that pass, as the mixer may hold its lock while calling.
        void setPaused(bool paused);
        // Blocks until the worker has taken in, and simulated with, everything handed to it
        // before the call; returns at once while paused, or when a pause comes first.
        void settle();

    private:
        struct Impl;
        explicit Simulation(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl;
    };
}

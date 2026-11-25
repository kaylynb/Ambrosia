#pragma once

#include "audio/acoustic_scene.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

// Baked acoustics for the raytraced sound mode: probes placed over the level's floors, reverb
// baked at each, and the paths sound takes between them. Baked from the level's static geometry
// with its movers left out, so a door is baked open; at run time the paths are checked against
// the scene with its movers where they are, and a closed door breaks those through it.
namespace dx::audio::acoustics {
    struct BakeSettings {
        // Probes: metres apart over every floor, and metres above it.
        float spacing = 2.0f;
        float height = 1.5f;
        // The most probes a level gets: past it the spacing widens until it fits. Pathing data
        // and the bake's memory grow with the square of the count (11_Paris_Cathedral at 4 m,
        // 2753 probes: 16 MB baked, 1.2 GB at most while baking).
        int32_t maxProbes = 3000;
        // Reverb at each probe.
        int32_t reverbRays = 8192;
        int32_t reverbBounces = 16;
        float reverbDuration = 2.0f;
        // Pathing: rays per probe pair are samples squared from spheres of radius metres; a pair
        // sees each other when more than threshold of them get through, up to visRange apart,
        // and paths are kept up to pathRange long.
        int32_t pathSamples = 4;
        float pathRadius = 0.5f;
        float pathThreshold = 0.1f;
        float visRange = 25.0f;
        float pathRange = 100.0f;
        // Threads the bake runs on, at most workers::threadCap(); 0 for the cap itself (all but
        // one of the machine's unless a setting says otherwise).
        int32_t threads = 0;

        bool operator==(const BakeSettings&) const = default;
    };

    struct Probe {
        Vector location;
        // Unreal units.
        float radius = 0;
    };

    enum class BakeStage : uint8_t {
        Probes,
        Reverb,
        Pathing,
        Saving,
    };

    const char* bakeStageName(BakeStage stage);

    // What a bake made: the probe batch as Steam Audio saves it, and what it holds.
    struct BakedData {
        std::vector<uint8_t> batch;
        std::vector<Probe> probes;
        // The spacing the probes were placed at, metres.
        float spacing = 0;
        // Probes the floor search placed before those outside the level's open space were left
        // out.
        size_t candidates = 0;
        uint64_t reverbBytes = 0;
        uint64_t pathBytes = 0;
        double probeMilliseconds = 0;
        double reverbMilliseconds = 0;
        double pathMilliseconds = 0;
    };

    // Probes over every floor of the level's open space: a grid of columns spacing apart, each
    // walked down through the BSP, with a probe height above each floor it meets (where open
    // space above meets solid below) that has room for it. Steam Audio's own floor search only
    // finds the highest surface in each column, which in an enclosed level is the roof.
    // The spacing used, widened from settings.spacing when the level would need more than
    // settings.maxProbes, goes to spacingUsed.
    std::vector<Probe> placeProbes(const Geometry& geometry, const BakeSettings& settings, float* spacingUsed = nullptr);

    // Progress as a stage and the fraction of it done; called on the baking thread.
    using BakeProgress = std::function<void(BakeStage, float)>;

    // Bakes geometry's static part. Blocks; only one bake runs at a time in the process, so a
    // second waits. A stop requested on stop cancels it soon after. Null when it was cancelled or failed,
    // with why in error.
    std::unique_ptr<BakedData> bake(const Geometry& geometry, const std::vector<Material>& materials, const BakeSettings& settings, const BakeProgress& progress, std::string* error, std::stop_token stop = {});
    // Whether this build and library can bake at all.
    bool bakingAvailable();
}

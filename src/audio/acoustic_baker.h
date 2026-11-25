#pragma once

#include "audio/acoustic_bake.h"
#include "audio/acoustic_cache.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// The baked data for the level being played, found in the cache or baked on a thread of its own
// while the raytraced mode carries on with its live simulation. The game thread hands it the
// level and takes the data from it once it is ready.
namespace dx::audio::acoustics {
    enum class BakeState : uint8_t {
        // No level, or no Steam Audio baking.
        Idle,
        Unavailable,
        // Reading and checking the cache.
        Checking,
        // The cache is missing, stale or damaged, and background baking is off.
        NotBaked,
        Baking,
        // In use: from the cache, or baked in this run.
        Ready,
        Failed,
    };

    const char* bakeStateName(BakeState state);

    struct BakeReport {
        BakeState state = BakeState::Idle;
        std::string map;
        // What the cache held when the level was entered, and why it was not used if it was not.
        CacheState cache = CacheState::Missing;
        std::string reason;
        // While baking: the stage and how far through it.
        BakeStage stage = BakeStage::Probes;
        float progress = 0;
        // The cache file, its size and when it was written (seconds ago), when there is one.
        std::filesystem::path path;
        uint64_t fileBytes = 0;
        double fileAgeSeconds = -1;
        size_t probes = 0;
        // The last bake's time, seconds.
        double bakeSeconds = 0;
        // The data in use, for drawing its probes.
        std::shared_ptr<const BakedData> data;
    };

    class Baker {
    public:
        Baker();
        ~Baker();
        Baker(const Baker&) = delete;
        Baker& operator=(const Baker&) = delete;

        // The level now played (by map name), or none with a null geometry. A bake under way for
        // another level is cancelled. Bakes on threads threads, and only when background is set
        // does it bake rather than just look in the cache.
        void setLevel(const std::string& map, std::shared_ptr<const Geometry> geometry, const std::vector<Material>& materials, const BakeSettings& settings, bool background);
        // Bakes the current level again, whatever the cache holds.
        void rebake();
        // The material table changed: data baked with another is stale, so it is dropped (the
        // mode then runs live) until the level is baked again.
        void setMaterials(const std::vector<Material>& materials);
        // Data newly ready for the current level since the last call, or null; a null with
        // dropped set means what was handed over before must no longer be used.
        std::shared_ptr<const BakedData> take(bool& dropped);
        BakeReport report() const;
        // Blocks until no job is running, for tests and the batch bake.
        void wait();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // Bakes one map's geometry and writes its cache file unless a valid one is there (or force).
    // For the batch bake; reports through progress.
    struct BatchResult {
        bool ok = false;
        bool skipped = false;
        std::string message;
        size_t probes = 0;
        float spacing = 0;
        uint64_t bytes = 0;
        double seconds = 0;
    };

    BatchResult bakeToCache(const std::string& map, const Geometry& geometry, const std::vector<Material>& materials, const BakeSettings& settings, bool force, const BakeProgress& progress);
}

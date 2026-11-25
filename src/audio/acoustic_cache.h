#pragma once

#include "audio/acoustic_bake.h"
#include "audio/acoustic_scene.h"
#include "core/derived_cache.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Baked acoustics on disk, as a consumer of the derived data cache (core/derived_cache.h): one
// file a map in AcousticCache under the user data folder. The key's level part is the hash of
// the scene as exported (its triangles with their materials, and its open space), so a change
// to the surface rules makes a bake stale as a change to the level does; its settings parts are
// the material table and the bake settings.
namespace dx::audio::acoustics {
    // Bumped whenever the file layout, or what a bake does with the same inputs, changes.
    constexpr uint32_t CacheFormat = 2;

    struct CacheKey {
        uint32_t format = CacheFormat;
        // Steam Audio's version as STEAMAUDIO_VERSION packs it, 0 in a build without it.
        uint32_t steamVersion = 0;
        // The level's static geometry, its triangles' materials as the surface rules gave them,
        // and where its open space is.
        uint64_t geometry = 0;
        // The material table's values.
        uint64_t materials = 0;
        // The probe and bake settings.
        uint64_t settings = 0;

        bool operator==(const CacheKey&) const = default;
    };

    CacheKey cacheKey(const Geometry& geometry, const std::vector<Material>& materials, const BakeSettings& settings);
    // Why a file with key found is stale for wanted, or empty when it is not.
    std::string staleReason(const CacheKey& found, const CacheKey& wanted);

    // Where a map's baked data is kept.
    std::filesystem::path cacheDirectory();
    std::filesystem::path cachePath(std::string_view map);

    using CacheState = derivedcache::State;

    const char* cacheStateName(CacheState state);

    struct CacheRead {
        CacheState state = CacheState::Missing;
        // Why it is stale or damaged.
        std::string reason;
        std::shared_ptr<BakedData> data;
    };

    // Reads and checks a file against the key it should have.
    CacheRead readCache(const std::filesystem::path& path, const CacheKey& wanted);
    // Writes a file whole, replacing any before only once the new one is complete.
    bool writeCache(const std::filesystem::path& path, const CacheKey& key, const BakedData& data, std::string* error);
}

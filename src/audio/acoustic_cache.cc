#include "audio/acoustic_cache.h"

#include "core/derived_cache.h"

#if DX_STEAM_AUDIO
#include <phonon.h>
#endif

#include <cstring>

namespace dx::audio::acoustics {
    namespace {
        const derivedcache::Consumer consumer = [] {
            derivedcache::Consumer c;
            c.name = "Acoustic";
            c.extension = ".dxbake";
            c.magic = { 'A', 'M', 'B', 'B', 'A', 'K', 'E', '\n' };
            c.otherVersion = "it was made by another version of Ambrosia's bake";
            c.otherLibrary = "it was made with another Steam Audio version";
            c.otherLevel = "the level's geometry has changed";
            c.otherSettings = { "the material table has changed", "the probe or bake settings have changed" };
            c.notOurs = "it is not an Ambrosia bake";
            return c;
        }();

        derivedcache::Key generalKey(const CacheKey& key)
        {
            return { key.format, key.steamVersion, key.geometry, { key.materials, key.settings } };
        }

        // The payload: what the bake holds, then the probes, then the batch.
        struct Header {
            uint64_t probes;
            uint64_t candidates;
            uint64_t batch;
            uint64_t reverbBytes;
            uint64_t pathBytes;
            double probeMilliseconds;
            double reverbMilliseconds;
            double pathMilliseconds;
            float spacing;
            uint32_t reserved;
        };

        struct StoredProbe {
            float x, y, z, radius;
        };

        // Limits that no real level comes near, so that a damaged length is not believed.
        constexpr uint64_t MaxProbes = 1u << 20;
        constexpr uint64_t MaxBatch = 1ull << 32;
    }

    CacheKey cacheKey(const Geometry& geometry, const std::vector<Material>& materials, const BakeSettings& settings)
    {
        CacheKey key;
#if DX_STEAM_AUDIO
        key.steamVersion = STEAMAUDIO_VERSION;
#endif
        derivedcache::Hash shape;
        shape.add(static_cast<uint64_t>(geometry.vertices.size()));
        for (const Vector& v : geometry.vertices) {
            shape.add(v.x);
            shape.add(v.y);
            shape.add(v.z);
        }
        shape.add(static_cast<uint64_t>(geometry.triangles.size()));
        for (size_t i = 0; i < geometry.triangles.size(); i++) {
            shape.add(geometry.triangles[i]);
            shape.add(geometry.triangleMaterial[i]);
        }
        shape.add(static_cast<uint64_t>(geometry.space.nodes.size()));
        for (const SpaceNode& node : geometry.space.nodes) {
            shape.add(node.plane);
            shape.add(node.front);
            shape.add(node.back);
            shape.add(node.zone);
            shape.add(node.csg);
        }
        shape.add(geometry.space.rootOutside);
        shape.add(geometry.space.skyZone);
        key.geometry = shape.value;

        derivedcache::Hash table;
        table.add(static_cast<uint64_t>(materials.size()));
        for (const Material& material : materials) {
            table.text(material.name);
            table.add(material.absorption);
            table.add(material.scattering);
            table.add(material.transmission);
        }
        key.materials = table.value;

        // Threads change how fast a bake runs, not what it makes.
        derivedcache::Hash bake;
        bake.add(settings.spacing);
        bake.add(settings.height);
        bake.add(settings.maxProbes);
        bake.add(settings.reverbRays);
        bake.add(settings.reverbBounces);
        bake.add(settings.reverbDuration);
        bake.add(settings.pathSamples);
        bake.add(settings.pathRadius);
        bake.add(settings.pathThreshold);
        bake.add(settings.visRange);
        bake.add(settings.pathRange);
        key.settings = bake.value;
        return key;
    }

    std::string staleReason(const CacheKey& found, const CacheKey& wanted)
    {
        return derivedcache::staleReason(consumer, generalKey(found), generalKey(wanted));
    }

    std::filesystem::path cacheDirectory()
    {
        return derivedcache::directory(consumer);
    }

    std::filesystem::path cachePath(std::string_view map)
    {
        return derivedcache::path(consumer, map);
    }

    const char* cacheStateName(CacheState state)
    {
        return derivedcache::stateName(state);
    }

    CacheRead readCache(const std::filesystem::path& path, const CacheKey& wanted)
    {
        derivedcache::Read file = derivedcache::read(consumer, path, generalKey(wanted));
        CacheRead read;
        read.state = file.state;
        read.reason = file.reason;
        if (file.state != CacheState::Valid) {
            return read;
        }
        auto damaged = [&](const char* why) {
            read.state = CacheState::Damaged;
            read.reason = why;
            return read;
        };
        const std::vector<uint8_t>& bytes = file.payload;
        if (bytes.size() < sizeof(Header)) {
            return damaged("it is too short");
        }
        Header header {};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (header.probes > MaxProbes || header.batch > MaxBatch) {
            return damaged("its lengths are impossible");
        }
        if (bytes.size() != sizeof(Header) + header.probes * sizeof(StoredProbe) + header.batch) {
            return damaged("it is cut short or has extra bytes");
        }
        auto data = std::make_shared<BakedData>();
        size_t offset = sizeof(Header);
        data->probes.resize(header.probes);
        for (Probe& probe : data->probes) {
            StoredProbe entry {};
            std::memcpy(&entry, bytes.data() + offset, sizeof(entry));
            offset += sizeof(entry);
            probe.location = { entry.x, entry.y, entry.z };
            probe.radius = entry.radius;
        }
        data->batch.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + header.batch));
        data->candidates = header.candidates;
        data->reverbBytes = header.reverbBytes;
        data->pathBytes = header.pathBytes;
        data->probeMilliseconds = header.probeMilliseconds;
        data->reverbMilliseconds = header.reverbMilliseconds;
        data->pathMilliseconds = header.pathMilliseconds;
        data->spacing = header.spacing;
        read.data = std::move(data);
        return read;
    }

    bool writeCache(const std::filesystem::path& path, const CacheKey& key, const BakedData& data, std::string* error)
    {
        Header header {};
        header.probes = data.probes.size();
        header.candidates = data.candidates;
        header.batch = data.batch.size();
        header.reverbBytes = data.reverbBytes;
        header.pathBytes = data.pathBytes;
        header.probeMilliseconds = data.probeMilliseconds;
        header.reverbMilliseconds = data.reverbMilliseconds;
        header.pathMilliseconds = data.pathMilliseconds;
        header.spacing = data.spacing;
        std::vector<uint8_t> bytes(sizeof(header));
        std::memcpy(bytes.data(), &header, sizeof(header));
        for (const Probe& probe : data.probes) {
            StoredProbe stored { probe.location.x, probe.location.y, probe.location.z, probe.radius };
            const auto* p = reinterpret_cast<const uint8_t*>(&stored);
            bytes.insert(bytes.end(), p, p + sizeof(stored));
        }
        bytes.insert(bytes.end(), data.batch.begin(), data.batch.end());
        return derivedcache::write(consumer, path, generalKey(key), bytes, error);
    }
}

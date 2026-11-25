#include "audio/acoustic_bake.h"
#include "audio/acoustic_baker.h"
#include "audio/acoustic_cache.h"
#include "audio/acoustic_scene.h"
#include "audio/acoustics.h"
#include "audio/audio.h"
#include "audio/raytraced.h"
#include "core/config.h"
#include "core/objects.h"
#include "engine/resources.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
    using dx::audio::acoustics::BakedData;
    using dx::audio::acoustics::Geometry;
    using dx::audio::acoustics::SurfaceInfo;

    void addBox(Geometry& geometry, const dx::Vector& centre, const dx::Vector& half, const SurfaceInfo& info, bool inward)
    {
        size_t surface = geometry.surfaceIndex(info);
        float sign = inward ? -1.0f : 1.0f;
        dx::Vector lo = centre - half;
        dx::Vector hi = centre + half;
        geometry.addPolygon({ { lo.x, lo.y, lo.z }, { lo.x, hi.y, lo.z }, { lo.x, hi.y, hi.z }, { lo.x, lo.y, hi.z } }, { -sign, 0, 0 }, surface);
        geometry.addPolygon({ { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { hi.x, hi.y, hi.z }, { hi.x, lo.y, hi.z } }, { sign, 0, 0 }, surface);
        geometry.addPolygon({ { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, lo.y, hi.z }, { lo.x, lo.y, hi.z } }, { 0, -sign, 0 }, surface);
        geometry.addPolygon({ { lo.x, hi.y, lo.z }, { hi.x, hi.y, lo.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } }, { 0, sign, 0 }, surface);
        geometry.addPolygon({ { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z } }, { 0, 0, -sign }, surface);
        geometry.addPolygon({ { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } }, { 0, 0, sign }, surface);
    }

    enum class Partition {
        None,
        Open,
        Sealed,
    };

    // A concrete hall 1600 by 800 units and 256 high around the listener at the origin, whose
    // ear is 80 units above the floor. A partition across x = 400 hides a source at (800, 0, 0);
    // when open, it leaves a gap at the far side, y from 400 to 600, to the listener's right.
    std::shared_ptr<Geometry> hall(Partition partition)
    {
        SurfaceInfo concrete { "Concrete", "Wall", 0, false };
        auto geometry = std::make_shared<Geometry>();
        addBox(*geometry, { 400, 200, 48 }, { 800, 400, 128 }, concrete, true);
        if (partition == Partition::Open) {
            addBox(*geometry, { 400, 100, 48 }, { 8, 300, 128 }, concrete, false);
        } else if (partition == Partition::Sealed) {
            addBox(*geometry, { 400, 200, 48 }, { 8, 400, 128 }, concrete, false);
        }
        return geometry;
    }

    const dx::Vector HiddenSource { 800, 0, 0 };

    std::shared_ptr<const BakedData> bakeHall(const Geometry& geometry)
    {
        std::string error;
        std::unique_ptr<BakedData> baked = dx::audio::acoustics::bake(geometry, dx::audio::acoustics::defaultMaterials(), {}, {}, &error);
        INFO(error);
        REQUIRE(baked);
        return std::shared_ptr<const BakedData>(std::move(baked));
    }

    // A second of looped white noise as a mono 44.1 kHz wave with a smpl loop.
    std::vector<uint8_t> noiseWave()
    {
        std::vector<uint8_t> bytes;
        auto u32 = [&bytes](uint32_t v) {
            for (int i = 0; i < 4; i++) {
                bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
            }
        };
        auto u16 = [&bytes](uint16_t v) {
            bytes.push_back(static_cast<uint8_t>(v));
            bytes.push_back(static_cast<uint8_t>(v >> 8));
        };
        const uint32_t frames = 44100;
        bytes.insert(bytes.end(), { 'R', 'I', 'F', 'F' });
        u32(36 + frames * 2 + 68);
        bytes.insert(bytes.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
        u32(16);
        u16(1);
        u16(1);
        u32(44100);
        u32(88200);
        u16(2);
        u16(16);
        bytes.insert(bytes.end(), { 'd', 'a', 't', 'a' });
        u32(frames * 2);
        uint32_t state = 7;
        for (uint32_t i = 0; i < frames; i++) {
            state = state * 1664525u + 1013904223u;
            u16(static_cast<uint16_t>(static_cast<int16_t>((static_cast<int32_t>(state >> 16) - 32768) / 2)));
        }
        bytes.insert(bytes.end(), { 's', 'm', 'p', 'l', 60, 0, 0, 0 });
        for (uint32_t i = 0; i < 15; i++) {
            u32(i == 7 ? 1u : (i == 12 ? frames - 1 : 0u));
        }
        return bytes;
    }

    std::string audioSection()
    {
        return dx::config::system().get("Engine.Engine", "AudioDevice").value_or("Galaxy.GalaxyAudioSubsystem");
    }

    // An offline mixer in the raytraced mode at full volume, needing no game data.
    class RaytracedMixer {
    public:
        RaytracedMixer()
        {
            dx::config::system().set(audioSection(), "AmbrosiaRaytracedAudio", "True");
            REQUIRE(dx::audio::initializeOffline(44100));
            dx::audio::setVolumes(1.0f, 1.0f, 1.0f);
            // The reverb controls last for the run, whatever test set them: the simulated reverb.
            dx::audio::ReverbControls controls = dx::audio::reverbControls();
            controls.authored = false;
            controls.level = 0.8f;
            controls.reflections = {};
            dx::audio::setReverbControls(controls);
            dx::audio::setBakedUse({});
        }

        ~RaytracedMixer()
        {
            dx::audio::shutdown();
            dx::audio::setVolumes(before.sound, before.music, before.speech);
        }

        RaytracedMixer(const RaytracedMixer&) = delete;
        RaytracedMixer& operator=(const RaytracedMixer&) = delete;

    private:
        dx::test::ConfigSnapshot settings;
        dx::audio::Volumes before = dx::audio::volumes();
    };

    struct Heard {
        double level = 0;
        double left = 0;
        double right = 0;
        // Energy of the sample-to-sample difference over energy: higher for a brighter sound.
        double brightness = 0;
        dx::audio::VoiceStatus status;
    };

    // Plays noise at source to the listener at the origin facing +x, in geometry (or the scene as
    // it is, given null) with baked data (or none), using what use says, and measures half a
    // second of it.
    Heard hear(std::shared_ptr<const Geometry> geometry, std::shared_ptr<const BakedData> baked, const dx::Vector& source, dx::audio::acoustics::BakedUse use)
    {
        if (geometry) {
            dx::audio::setAcousticGeometry(std::move(geometry));
        }
        dx::audio::setAcousticBaked(std::move(baked));
        dx::audio::setBakedUse(use);
        dx::USound noise;
        noise.bytes = noiseWave();
        dx::audio::update(nullptr, {}, {}, 0.0f);
        int32_t id = dx::audio::playSound(&noise, nullptr, dx::audio::SlotNone, source, 1.0f, 4000.0f, 1.0f, true);
        REQUIRE(id != 0);
        dx::audio::update(nullptr, {}, {}, 0.0f);
        dx::audio::settleAcoustics();
        dx::audio::settleAcoustics();
        std::vector<float> buffer(4096 * 2);
        dx::audio::render(buffer.data(), 4096);
        buffer.assign(22050 * 2, 0.0f);
        if (std::getenv("AMBROSIA_TRACE_PATHED")) {
            for (size_t done = 0; done < 22050; done += 2205) {
                dx::audio::render(buffer.data() + done * 2, 2205);
                for (const dx::audio::VoiceStatus& status : dx::audio::voices()) {
                    std::printf("  %zu: found %d pathed %.3f occl %.3f trans %.3f\n", done, status.pathFound ? 1 : 0, static_cast<double>(status.pathed[0]), static_cast<double>(status.occlusion), static_cast<double>(status.transmission[0]));
                }
            }
        } else {
            dx::audio::render(buffer.data(), 22050);
        }
        Heard heard;
        for (const dx::audio::VoiceStatus& status : dx::audio::voices()) {
            if (status.id == id) {
                heard.status = status;
            }
        }
        double change = 0;
        for (size_t i = 1; i < 22050; i++) {
            double l = static_cast<double>(buffer[i * 2]);
            double r = static_cast<double>(buffer[i * 2 + 1]);
            heard.left += l * l;
            heard.right += r * r;
            for (size_t ear = 0; ear < 2; ear++) {
                double d = static_cast<double>(buffer[i * 2 + ear]) - static_cast<double>(buffer[(i - 1) * 2 + ear]);
                change += d * d;
            }
        }
        double energy = heard.left + heard.right;
        heard.level = std::sqrt(energy / 44100.0);
        heard.brightness = energy > 0 ? change / energy : 0;
        // Rung out, so the reverb of a loud sound is not heard in the next one measured.
        dx::audio::stopSound(id);
        buffer.assign(88200 * 2, 0.0f);
        dx::audio::render(buffer.data(), 88200);
        return heard;
    }

    void report(const char* what, const Heard& heard)
    {
        std::printf("%s: level %.4f (%.1f dB) left %.3g right %.3g brightness %.3f occlusion %.2f path %d length %.0f way %.2f %.2f %.2f pathed %.3f %.3f %.3f\n", what, heard.level, 20.0 * std::log10(std::max(heard.level, 1e-9)), heard.left, heard.right, heard.brightness, static_cast<double>(heard.status.occlusion), heard.status.pathFound ? 1 : 0, static_cast<double>(heard.status.pathLength), static_cast<double>(heard.status.pathDirection.x), static_cast<double>(heard.status.pathDirection.y), static_cast<double>(heard.status.pathDirection.z), static_cast<double>(heard.status.pathed[0]), static_cast<double>(heard.status.pathed[1]), static_cast<double>(heard.status.pathed[2]));
    }

    constexpr dx::audio::acoustics::BakedUse PathingOn { true, true };
    constexpr dx::audio::acoustics::BakedUse PathingOff { true, false };

    // XDG_DATA_HOME points the user data folder somewhere else until this goes.
    class UserData {
    public:
        explicit UserData(const std::filesystem::path& path)
        {
            if (const char* value = std::getenv("XDG_DATA_HOME")) {
                previous = value;
            }
            set(path.string());
        }

        ~UserData()
        {
            set(previous.value_or(""));
        }

        UserData(const UserData&) = delete;
        UserData& operator=(const UserData&) = delete;

    private:
        static void set(const std::string& value)
        {
#ifdef _WIN32
            _putenv_s("XDG_DATA_HOME", value.c_str());
#else
            if (value.empty()) {
                unsetenv("XDG_DATA_HOME");
            } else {
                setenv("XDG_DATA_HOME", value.c_str(), 1);
            }
#endif
        }

        std::optional<std::string> previous;
    };
}

TEST_CASE("probes cover the floors of a scene without a BSP and keep clear of what is above")
{
    auto geometry = hall(Partition::Open);
    std::vector<dx::audio::acoustics::Probe> probes = dx::audio::acoustics::placeProbes(*geometry, {});
    // 1600 by 800 units at 2 m (105 units) is 15 by 7 columns, less those on the partition.
    CHECK(probes.size() >= 90);
    for (const auto& probe : probes) {
        CHECK(probe.location.z == Catch::Approx(-80 + 1.5f * dx::audio::acoustics::UnitsPerMetre).margin(1));
        CHECK(std::fabs(probe.location.x - 400) > 8);
    }
}

TEST_CASE("a level that would need more probes than the cap gets them wider apart")
{
    auto geometry = hall(Partition::None);
    dx::audio::acoustics::BakeSettings settings;
    float spacing = 0;
    size_t full = dx::audio::acoustics::placeProbes(*geometry, settings, &spacing).size();
    CHECK(spacing == 2.0f);
    settings.maxProbes = 30;
    std::vector<dx::audio::acoustics::Probe> capped = dx::audio::acoustics::placeProbes(*geometry, settings, &spacing);
    CHECK(full > 30);
    CHECK(capped.size() <= 30);
    CHECK(capped.size() >= 10);
    CHECK(spacing > 2.0f);
    CHECK(capped.front().radius == Catch::Approx(spacing * dx::audio::acoustics::UnitsPerMetre));
}

TEST_CASE("cache keys change with the geometry and the materials and the settings but not the threads")
{
    using dx::audio::acoustics::cacheKey;
    using dx::audio::acoustics::staleReason;
    auto geometry = hall(Partition::Open);
    std::vector<dx::audio::acoustics::Material> table = dx::audio::acoustics::defaultMaterials();
    dx::audio::acoustics::BakeSettings settings;
    dx::audio::acoustics::CacheKey key = cacheKey(*geometry, table, settings);
    CHECK(key == cacheKey(*geometry, table, settings));

    Geometry moved = *geometry;
    moved.vertices[3].z += 1;
    CHECK(staleReason(key, cacheKey(moved, table, settings)) == "the level's geometry has changed");
    Geometry rematerialled = *geometry;
    rematerialled.triangleMaterial[0] = dx::audio::acoustics::materialIndex("metal");
    CHECK(staleReason(key, cacheKey(rematerialled, table, settings)) == "the level's geometry has changed");
    Geometry respaced = *geometry;
    respaced.space.nodes.push_back({});
    CHECK(staleReason(key, cacheKey(respaced, table, settings)) == "the level's geometry has changed");

    std::vector<dx::audio::acoustics::Material> tuned = table;
    tuned[1].absorption[0] += 0.01f;
    CHECK(staleReason(key, cacheKey(*geometry, tuned, settings)) == "the material table has changed");

    dx::audio::acoustics::BakeSettings closer = settings;
    closer.spacing = 1.5f;
    CHECK(staleReason(key, cacheKey(*geometry, table, closer)) == "the probe or bake settings have changed");
    dx::audio::acoustics::BakeSettings fewer = settings;
    fewer.maxProbes = 1000;
    CHECK(staleReason(key, cacheKey(*geometry, table, fewer)) == "the probe or bake settings have changed");
    dx::audio::acoustics::BakeSettings faster = settings;
    faster.threads = 7;
    CHECK(cacheKey(*geometry, table, faster) == key);

    dx::audio::acoustics::CacheKey otherSteam = key;
    otherSteam.steamVersion ^= 1;
    CHECK(staleReason(otherSteam, key) == "it was made with another Steam Audio version");
    dx::audio::acoustics::CacheKey otherFormat = key;
    otherFormat.format++;
    CHECK(staleReason(otherFormat, key) == "it was made by another version of Ambrosia's bake");
}

TEST_CASE("a cache file reads back whole and a stale or damaged or missing one is refused")
{
    dx::test::TempDir folder("acoustic-cache");
    std::filesystem::path path = folder / "test.dxbake";
    auto geometry = hall(Partition::Open);
    dx::audio::acoustics::CacheKey key = dx::audio::acoustics::cacheKey(*geometry, dx::audio::acoustics::defaultMaterials(), {});
    BakedData data;
    data.probes = { { { 1, 2, 3 }, 105 }, { { 4, 5, 6 }, 105 } };
    data.candidates = 2;
    for (int i = 0; i < 1000; i++) {
        data.batch.push_back(static_cast<uint8_t>(i * 7));
    }
    data.reverbBytes = 10;
    data.pathBytes = 990;
    std::string error;
    REQUIRE(dx::audio::acoustics::writeCache(path, key, data, &error));
    CHECK_FALSE(std::filesystem::exists(folder / "test.dxbake.partial"));

    dx::audio::acoustics::CacheRead read = dx::audio::acoustics::readCache(path, key);
    REQUIRE(read.state == dx::audio::acoustics::CacheState::Valid);
    CHECK(read.data->batch == data.batch);
    REQUIRE(read.data->probes.size() == 2);
    CHECK(read.data->probes[1].location.y == 5);
    CHECK(read.data->pathBytes == 990);

    dx::audio::acoustics::CacheKey other = key;
    other.materials++;
    read = dx::audio::acoustics::readCache(path, other);
    CHECK(read.state == dx::audio::acoustics::CacheState::Stale);
    CHECK(read.reason == "the material table has changed");
    CHECK_FALSE(read.data);

    CHECK(dx::audio::acoustics::readCache(folder / "none.dxbake", key).state == dx::audio::acoustics::CacheState::Missing);

    std::vector<char> bytes;
    {
        std::ifstream in(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    auto writeBytes = [&](const std::vector<char>& content) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    };
    std::vector<char> flipped = bytes;
    flipped[flipped.size() / 2] ^= 0x10;
    writeBytes(flipped);
    read = dx::audio::acoustics::readCache(path, key);
    CHECK(read.state == dx::audio::acoustics::CacheState::Damaged);
    CHECK(read.reason == "its checksum does not match");
    CHECK_FALSE(read.data);

    std::vector<char> cut(bytes.begin(), bytes.end() - 100);
    writeBytes(cut);
    read = dx::audio::acoustics::readCache(path, key);
    CHECK(read.state == dx::audio::acoustics::CacheState::Damaged);
    CHECK_FALSE(read.data);

    std::vector<char> foreign = bytes;
    foreign[0] = 'X';
    writeBytes(foreign);
    CHECK(dx::audio::acoustics::readCache(path, key).state == dx::audio::acoustics::CacheState::Damaged);
}

TEST_CASE("the baker uses a valid cache file and reports a damaged one without using it")
{
    if (!dx::audio::acoustics::bakingAvailable()) {
        SKIP("Steam Audio's baking is not available");
    }
    dx::test::TempDir folder("acoustic-baker");
    UserData userData(folder.path());
    auto geometry = hall(Partition::Open);
    std::vector<dx::audio::acoustics::Material> table = dx::audio::acoustics::defaultMaterials();
    REQUIRE(dx::audio::acoustics::cachePath("Test_Hall").parent_path() == folder / "ambrosia" / "AcousticCache");

    dx::audio::acoustics::Baker baker;
    // Nothing there and background baking off: reported, nothing handed over.
    baker.setLevel("Test_Hall", geometry, table, {}, false);
    baker.wait();
    bool dropped = false;
    CHECK_FALSE(baker.take(dropped));
    CHECK(baker.report().state == dx::audio::acoustics::BakeState::NotBaked);
    CHECK(baker.report().cache == dx::audio::acoustics::CacheState::Missing);

    // Baked in the background, written, handed over.
    baker.setLevel("Test_Hall", geometry, table, {}, true);
    baker.wait();
    std::shared_ptr<const BakedData> baked = baker.take(dropped);
    REQUIRE(baked);
    dx::audio::acoustics::BakeReport report = baker.report();
    CHECK(report.state == dx::audio::acoustics::BakeState::Ready);
    CHECK(report.fileBytes > baked->batch.size());
    CHECK(report.fileAgeSeconds >= 0);

    // Entered again: found in the cache.
    baker.setLevel("Test_Hall", geometry, table, {}, false);
    baker.wait();
    std::shared_ptr<const BakedData> cached = baker.take(dropped);
    REQUIRE(cached);
    CHECK(cached->batch == baked->batch);
    CHECK(baker.report().cache == dx::audio::acoustics::CacheState::Valid);

    // Damaged on disk: refused and reported, and what was in use is dropped.
    {
        std::fstream file(dx::audio::acoustics::cachePath("Test_Hall"), std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(200);
        file.put('\x5a');
    }
    baker.setLevel("Test_Hall", geometry, table, {}, false);
    baker.wait();
    CHECK_FALSE(baker.take(dropped));
    CHECK(dropped);
    CHECK(baker.report().cache == dx::audio::acoustics::CacheState::Damaged);
    CHECK(baker.report().state == dx::audio::acoustics::BakeState::NotBaked);

    // A tuned material table makes the bake stale: dropped, and looked up again, not baked.
    baker.rebake();
    baker.wait();
    REQUIRE(baker.take(dropped));
    std::vector<dx::audio::acoustics::Material> tuned = table;
    tuned[1].absorption[0] += 0.05f;
    baker.setMaterials(tuned);
    baker.wait();
    CHECK_FALSE(baker.take(dropped));
    CHECK(dropped);
    CHECK(baker.report().cache == dx::audio::acoustics::CacheState::Stale);
    CHECK(baker.report().reason == "the material table has changed");
}

TEST_CASE("a bake saved and reloaded gives the same paths and reverb as the fresh one")
{
    if (!dx::audio::acoustics::bakingAvailable()) {
        SKIP("Steam Audio's baking is not available");
    }
    dx::test::TempDir folder("acoustic-reload");
    auto geometry = hall(Partition::Open);
    std::shared_ptr<const BakedData> fresh = bakeHall(*geometry);
    dx::audio::acoustics::CacheKey key = dx::audio::acoustics::cacheKey(*geometry, dx::audio::acoustics::defaultMaterials(), {});
    REQUIRE(dx::audio::acoustics::writeCache(folder / "hall.dxbake", key, *fresh, nullptr));
    dx::audio::acoustics::CacheRead read = dx::audio::acoustics::readCache(folder / "hall.dxbake", key);
    REQUIRE(read.state == dx::audio::acoustics::CacheState::Valid);

    struct Result {
        dx::audio::acoustics::PathResult path;
        std::array<float, 3> reverb {};
        bool reverbBaked = false;
    };

    auto simulate = [&](std::shared_ptr<const BakedData> data) {
        auto simulation = dx::audio::acoustics::Simulation::create(44100, 4);
        REQUIRE(simulation);
        simulation->setGeometry(geometry);
        simulation->setListener({ 0, 0, 0 }, {});
        simulation->setSources({ { 1, HiddenSource } });
        simulation->setBaked(std::move(data));
        simulation->settle();
        simulation->settle();
        Result result;
        std::unordered_map<int32_t, dx::audio::acoustics::PathResult> paths;
        uint64_t version = 0;
        REQUIRE(simulation->pathResults(paths, version));
        result.path = paths[1];
        REQUIRE(simulation->reverbTimes(result.reverb));
        result.reverbBaked = simulation->stats().reverbBaked;
        return result;
    };
    Result a = simulate(fresh);
    Result b = simulate(read.data);
    REQUIRE(a.path.found);
    CHECK(a.reverbBaked);
    CHECK(b.reverbBaked);
    CHECK(a.path.eq == b.path.eq);
    CHECK(a.path.field == b.path.field);
    CHECK(a.path.length == b.path.length);
    CHECK(a.reverb == b.reverb);
}

TEST_CASE("a listener or a source standing exactly on a probe is pathed without a fault")
{
    if (!dx::audio::acoustics::bakingAvailable()) {
        SKIP("Steam Audio's baking is not available");
    }
    auto geometry = hall(Partition::Open);
    std::shared_ptr<const BakedData> baked = bakeHall(*geometry);
    // The probes nearest the listener's side and the source's side of the partition.
    const dx::audio::acoustics::Probe* near = nullptr;
    const dx::audio::acoustics::Probe* far = nullptr;
    for (const auto& probe : baked->probes) {
        if (!near || (probe.location - dx::Vector { 0, 0, 0 }).size() < (near->location - dx::Vector { 0, 0, 0 }).size()) {
            near = &probe;
        }
        if (!far || (probe.location - HiddenSource).size() < (far->location - HiddenSource).size()) {
            far = &probe;
        }
    }
    auto simulation = dx::audio::acoustics::Simulation::create(44100, 4);
    REQUIRE(simulation);
    simulation->setGeometry(geometry);
    simulation->setListener(near->location, {});
    simulation->setSources({ { 1, far->location } });
    simulation->setBaked(baked);
    simulation->setTracedSource(1);
    simulation->settle();
    simulation->settle();
    std::unordered_map<int32_t, dx::audio::acoustics::PathResult> paths;
    uint64_t version = 0;
    REQUIRE(simulation->pathResults(paths, version));
    CHECK(paths[1].found);
}

TEST_CASE("a source round a corner is louder with pathing and heard from the opening")
{
    RaytracedMixer mixer;
    if (!dx::audio::raytracedActive() || !dx::audio::acoustics::bakingAvailable()) {
        SKIP("Steam Audio's simulation or baking is not available");
    }
    auto geometry = hall(Partition::Open);
    auto baked = bakeHall(*geometry);
    Heard without = hear(geometry, baked, HiddenSource, PathingOff);
    Heard with = hear(geometry, baked, HiddenSource, PathingOn);
    report("round the corner, pathing off", without);
    report("round the corner, pathing on", with);
    CHECK(without.status.occlusion < 0.1f);
    CHECK_FALSE(without.status.pathFound);
    REQUIRE(with.status.pathFound);
    // Louder by several decibels, and not as dull as through the wall.
    CHECK(with.level > without.level * 2.0);
    CHECK(with.brightness > without.brightness);
    // The way it arrives points to the gap (to the listener's front right), not through the
    // wall to the source straight ahead.
    CHECK(with.status.pathDirection.y > 0.5f);
    CHECK(with.status.pathDirection.x > 0.3f);
    // Heard more on the right, where the gap is, than without pathing.
    CHECK(with.right / with.left > without.right / without.left * 1.2);
    // The path is longer than the straight line, and Galaxy's curve takes that into account.
    CHECK(with.status.pathLength > 1100.0f);
}

TEST_CASE("a source with no open route is not made louder by pathing")
{
    RaytracedMixer mixer;
    if (!dx::audio::raytracedActive() || !dx::audio::acoustics::bakingAvailable()) {
        SKIP("Steam Audio's simulation or baking is not available");
    }
    auto geometry = hall(Partition::Sealed);
    auto baked = bakeHall(*geometry);
    Heard without = hear(geometry, baked, HiddenSource, PathingOff);
    Heard with = hear(geometry, baked, HiddenSource, PathingOn);
    report("sealed, pathing off", without);
    report("sealed, pathing on", with);
    CHECK_FALSE(with.status.pathFound);
    CHECK(with.status.pathed == std::array<float, 3> {});
    CHECK(with.level == Catch::Approx(without.level).epsilon(0.05));
}

TEST_CASE("a source in plain view is not changed by pathing")
{
    RaytracedMixer mixer;
    if (!dx::audio::raytracedActive() || !dx::audio::acoustics::bakingAvailable()) {
        SKIP("Steam Audio's simulation or baking is not available");
    }
    auto geometry = hall(Partition::None);
    auto baked = bakeHall(*geometry);
    Heard without = hear(geometry, baked, HiddenSource, PathingOff);
    Heard with = hear(geometry, baked, HiddenSource, PathingOn);
    report("plain view, pathing off", without);
    report("plain view, pathing on", with);
    // A source in plain view is not pathed at all (its path would be the straight line) and so
    // gains nothing.
    CHECK_FALSE(with.status.pathFound);
    CHECK(with.status.pathed == std::array<float, 3> {});
    CHECK(with.level == Catch::Approx(without.level).epsilon(0.01));
    CHECK(with.left == Catch::Approx(without.left).epsilon(0.02));
    CHECK(with.right == Catch::Approx(without.right).epsilon(0.02));
}

TEST_CASE("a closed door breaks the baked path through it and an open one does not")
{
    RaytracedMixer mixer;
    if (!dx::audio::raytracedActive() || !dx::audio::acoustics::bakingAvailable()) {
        SKIP("Steam Audio's simulation or baking is not available");
    }
    // The gap gets a door, a mover: baked without it, as a level's movers are.
    auto geometry = hall(Partition::Open);
    auto baked = bakeHall(*geometry);
    auto door = std::make_shared<Geometry>();
    addBox(*door, { 0, 0, 0 }, { 8, 100, 128 }, { "Wood", "Door", 0, false }, false);
    geometry->movers.push_back({ 7, door });
    dx::audio::acoustics::MoverPose closed;
    closed.id = 7;
    closed.location = { 400, 500, 48 };
    dx::audio::acoustics::MoverPose open = closed;
    open.location.z += 300;

    dx::audio::setAcousticMoverPoses({ open });
    Heard opened = hear(geometry, baked, HiddenSource, PathingOn);
    dx::audio::setAcousticMoverPoses({ closed });
    Heard shut = hear(nullptr, baked, HiddenSource, PathingOn);
    dx::audio::setBakedUse(PathingOff);
    Heard shutWithout = hear(nullptr, baked, HiddenSource, PathingOff);
    report("door open, pathing on", opened);
    report("door closed, pathing on", shut);
    report("door closed, pathing off", shutWithout);
    REQUIRE(opened.status.pathFound);
    CHECK(opened.status.pathed[0] > 0.3f);
    // Closed, the path through the door fails validation and nothing else gets round: no
    // louder than the occluded direct sound.
    CHECK(shut.status.pathed == std::array<float, 3> {});
    CHECK(shut.level == Catch::Approx(shutWithout.level).epsilon(0.05));
    CHECK(opened.level > shut.level * 2.0);
    dx::audio::setAcousticMoverPoses({});
}

TEST_CASE("baked data arriving mid-play takes over the reverb with no gap")
{
    RaytracedMixer mixer;
    if (!dx::audio::raytracedActive() || !dx::audio::acoustics::bakingAvailable()) {
        SKIP("Steam Audio's simulation or baking is not available");
    }
    auto geometry = hall(Partition::None);
    auto baked = bakeHall(*geometry);
    dx::audio::setAcousticGeometry(geometry);
    dx::audio::setAcousticBaked(nullptr);
    dx::audio::setBakedUse(PathingOn);
    dx::USound noise;
    noise.bytes = noiseWave();
    dx::audio::update(nullptr, {}, {}, 0.0f);
    int32_t id = dx::audio::playSound(&noise, nullptr, dx::audio::SlotNone, { 300, 0, 0 }, 1.0f, 4000.0f, 1.0f, true);
    REQUIRE(id != 0);
    dx::audio::update(nullptr, {}, {}, 0.0f);
    dx::audio::settleAcoustics();
    std::vector<float> buffer(512 * 2);
    for (int block = 0; block < 40; block++) {
        dx::audio::render(buffer.data(), 512);
    }
    CHECK_FALSE(dx::audio::acousticStatus().simulation.reverbBaked);
    std::array<float, 3> live = dx::audio::reverbControls().times;
    CHECK(live[1] > 0);
    // The sound stops; its reverb rings on, and the baked data arrives as it does.
    dx::audio::stopSound(id);
    std::vector<double> energy;
    for (int block = 0; block < 60; block++) {
        if (block == 4) {
            dx::audio::setAcousticBaked(baked);
            dx::audio::update(nullptr, {}, {}, 0.0f);
            dx::audio::settleAcoustics();
        }
        dx::audio::render(buffer.data(), 512);
        double sum = 0;
        for (float sample : buffer) {
            sum += static_cast<double>(sample) * static_cast<double>(sample);
        }
        energy.push_back(sum);
    }
    CHECK(dx::audio::acousticStatus().simulation.reverbBaked);
    std::array<float, 3> fromBake = dx::audio::reverbControls().times;
    std::printf("reverb times live %.2f %.2f %.2f s, baked %.2f %.2f %.2f s\n", static_cast<double>(live[0]), static_cast<double>(live[1]), static_cast<double>(live[2]), static_cast<double>(fromBake[0]), static_cast<double>(fromBake[1]), static_cast<double>(fromBake[2]));
    CHECK(fromBake[1] > 0);
    // The tail decays without a block falling away to nothing and coming back: each block keeps
    // at least a fifth of the one before while the tail is still well above silence.
    for (size_t block = 2; block < energy.size(); block++) {
        if (energy[block - 1] < energy[1] * 1e-4) {
            break;
        }
        INFO("block " << block << ": " << energy[block] << " after " << energy[block - 1]);
        CHECK(energy[block] > energy[block - 1] * 0.2);
    }
}

// Measurements of the raytraced sound mode in real levels, not run by default
// ("[.acoustic-measure]"): the reverb level that matches the authored zone reverb, and the
// mode's cost in the heaviest level.

#include "audio/acoustic_scene.h"
#include "test_paths.h"
#include "audio/audio.h"
#include "audio/raytraced.h"
#include "core/config.h"
#include "core/objects.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/console.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {
    std::vector<uint8_t> measureNoise(uint32_t seed)
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
        uint32_t state = seed;
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

    dx::UObject* reverbZone(dx::World& world)
    {
        for (dx::UObject* actor : world.level->actors) {
            if (actor && actor->data && actor->isA(dx::engineClasses().zoneInfo) && !actor->isA(dx::engineClasses().levelInfo) && dx::zoneinfo::bReverbZone(actor)) {
                return actor;
            }
        }
        return nullptr;
    }

    double energyOf(const std::vector<float>& samples)
    {
        double sum = 0;
        for (float sample : samples) {
            sum += static_cast<double>(sample) * static_cast<double>(sample);
        }
        return sum;
    }
}

TEST_CASE("the raytraced reverb level that matches the authored zone reverb", "[.acoustic-measure][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::ConfigSnapshot settings;
    dx::config::system().set("Galaxy.GalaxyAudioSubsystem", "AmbrosiaRaytracedAudio", "True");
    dx::config::system().set("Galaxy.GalaxyAudioSubsystem", "UseReverb", "True");
    dx::test::OfflineAudio audio;
    if (!dx::audio::raytracedActive()) {
        SKIP("Steam Audio's simulation is not available");
    }
    double product = 1;
    int maps = 0;
    for (const char* map : { "06_HongKong_WanChai_Garage", "03_NYC_AirfieldHeliBase" }) {
        dx::objects::unloadPackage(dx::Name(map));
        dx::World world;
        REQUIRE(world.loadMap(map));
        dx::UObject* player = world.player();
        REQUIRE(player);
        dx::UObject* zone = reverbZone(world);
        REQUIRE(zone);
        dx::Vector spot = dx::actor::Location(zone);
        char command[128];
        std::snprintf(command, sizeof(command), "teleport %.0f %.0f %.0f 0", static_cast<double>(spot.x), static_cast<double>(spot.y), static_cast<double>(spot.z));
        dx::console::execute(player, command);
        dx::USound noise;
        noise.bytes = measureNoise(3);
        auto settle = [&] {
            for (int i = 0; i < 3; i++) {
                world.tick(1.0f / 30.0f);
                dx::audio::update(&world, dx::actor::Location(player), {}, 1.0f / 30.0f);
            }
            dx::audio::settleAcoustics();
        };
        settle();
        // At the listener, so nothing stands between them to occlude it and its dry level holds
        // still (volumetric occlusion samples at random). An unplaced sound never reaches the reverb.
        int32_t id = dx::audio::playSound(&noise, nullptr, dx::audio::SlotNone, dx::actor::Location(player), 1.0f, 2000.0f, 1.0f, true);
        REQUIRE(id != 0);
        // The level's own ambient sounds are silenced, so only the noise is heard and reverberated.
        dx::audio::setVoiceSoloed(id, true);
        dx::audio::setVolumes(1.0f, 0.0f, 1.0f);
        settle();
        auto measure = [&](float level, bool authored) {
            dx::audio::ReverbControls controls = dx::audio::reverbControls();
            controls.level = level;
            controls.authored = authored;
            dx::audio::setReverbControls(controls);
            std::vector<float> buffer(44100 * 2);
            dx::audio::render(buffer.data(), 44100);
            dx::audio::render(buffer.data(), 44100);
            return buffer;
        };
        // The noise loops every 44100 frames and its dry path holds still, so the reverb is what
        // a render with it differs from one without, sample by sample. (Galaxy's reverb passes an
        // inverted share of the send, so its energy cannot be found by subtracting energies.)
        auto difference = [](const std::vector<float>& with, const std::vector<float>& without) {
            double sum = 0;
            for (size_t i = 0; i < with.size(); i++) {
                double d = static_cast<double>(with[i]) - static_cast<double>(without[i]);
                sum += d * d;
            }
            return sum;
        };
        std::vector<float> dryBuffer = measure(0.0f, false);
        std::vector<float> dryAgain = measure(0.0f, false);
        double dry = energyOf(dryBuffer);
        double noiseFloor = difference(dryAgain, dryBuffer);
        double simulated = difference(measure(1.0f, false), dryBuffer);
        double authored = difference(measure(0.0f, true), dryBuffer);
        std::printf("  dry repeat difference %.2e of dry\n", noiseFloor / dry);
        dx::audio::ReverbSettings zoneSettings = dx::audio::reverbSettings();
        std::array<float, 3> times = dx::audio::acousticStatus().simulation.reverbTimes;
        double level = std::sqrt(authored / simulated);
        std::printf("%s at %s: zone reverb volume %.2f; simulated RT60 %.2f %.2f %.2f s; reverb energy over dry: authored %.3f, simulated at level 1 %.3f; matching level %.3f\n", map, zone->name.c_str(), static_cast<double>(zoneSettings.volume), static_cast<double>(times[0]), static_cast<double>(times[1]), static_cast<double>(times[2]), authored / dry, simulated / dry, level);
        product *= level;
        maps++;
        dx::audio::stopSound(id);
        dx::objects::unloadPackage(dx::Name(map));
    }
    std::printf("geometric mean of the matching levels: %.3f\n", std::pow(product, 1.0 / maps));
    audio.shutdown();
}

TEST_CASE("switching into the raytraced mode in 15_Area51_Page, first and again", "[.acoustic-measure][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::ConfigSnapshot settings;
    dx::config::system().set("Galaxy.GalaxyAudioSubsystem", "AmbrosiaHRTF", "True");
    dx::config::system().set("Galaxy.GalaxyAudioSubsystem", "AmbrosiaRaytracedAudio", "False");
    dx::test::OfflineAudio audio;
    dx::objects::unloadPackage(dx::Name("15_Area51_Page"));
    dx::World world;
    REQUIRE(world.loadMap("15_Area51_Page"));
    dx::UObject* player = world.player();
    REQUIRE(player);
    auto milliseconds = [](std::chrono::steady_clock::time_point since) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
    };
    auto mode = [](bool raytraced) {
        dx::config::system().set("Galaxy.GalaxyAudioSubsystem", "AmbrosiaRaytracedAudio", raytraced ? "True" : "False");
        auto start = std::chrono::steady_clock::now();
        dx::audio::applyMenuSettings();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    double firstSwitch = mode(true);
    if (!dx::audio::raytracedActive()) {
        SKIP("Steam Audio's simulation is not available");
    }
    auto start = std::chrono::steady_clock::now();
    dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
    double firstUpdate = milliseconds(start);
    dx::audio::settleAcoustics();
    double firstReady = milliseconds(start);
    dx::audio::AcousticStatus first = dx::audio::acousticStatus();
    double off = mode(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t asleep = dx::audio::acousticStatus().simulation.passes;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(dx::audio::acousticStatus().simulation.passes == asleep);
    double again = mode(true);
    start = std::chrono::steady_clock::now();
    dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
    double againUpdate = milliseconds(start);
    dx::audio::settleAcoustics();
    double againReady = milliseconds(start);
    dx::audio::AcousticStatus second = dx::audio::acousticStatus();
    // The scene was kept: not built again.
    CHECK(second.simulation.buildMilliseconds == first.simulation.buildMilliseconds);
    CHECK(second.simulation.triangles == first.simulation.triangles);
    std::printf("first switch on: %.1f ms to switch (making the simulator and effects), then %.1f ms for the update that exports the level, scene ready %.1f ms after (build %.1f ms on the worker)\n", firstSwitch, firstUpdate, firstReady, first.simulation.buildMilliseconds);
    std::printf("switch off: %.2f ms; worker idle while off; switch on again: %.2f ms, then %.2f ms update, simulating again %.1f ms after, scene kept\n", off, again, againUpdate, againReady);
    dx::objects::unloadPackage(dx::Name("15_Area51_Page"));
    audio.shutdown();
}

TEST_CASE("the raytraced mode's cost in 15_Area51_Page with every channel playing", "[.acoustic-measure][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::ConfigSnapshot settings;
    dx::config::system().set("Galaxy.GalaxyAudioSubsystem", "AmbrosiaRaytracedAudio", "True");
    dx::config::system().set("Galaxy.GalaxyAudioSubsystem", "EffectsChannels", "64");
    dx::test::OfflineAudio audio;
    if (!dx::audio::raytracedActive()) {
        SKIP("Steam Audio's simulation is not available");
    }
    dx::objects::unloadPackage(dx::Name("15_Area51_Page"));
    dx::World world;
    REQUIRE(world.loadMap("15_Area51_Page"));
    dx::UObject* player = world.player();
    REQUIRE(player);
    auto exportStart = std::chrono::steady_clock::now();
    dx::audio::acoustics::Geometry geometry = dx::audio::acoustics::levelGeometry(world.level);
    double exportMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - exportStart).count();
    dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
    dx::audio::settleAcoustics();
    dx::audio::AcousticStatus status = dx::audio::acousticStatus();
    std::printf("15_Area51_Page: %zu triangles and %zu movers; export %.1f ms on the game thread, scene build %.1f ms on the worker (%s)\n", status.simulation.triangles, status.simulation.movers, exportMs, status.simulation.buildMilliseconds, status.simulation.embree ? "Embree" : "built-in");
    std::vector<dx::USound> sounds(64);
    dx::Vector centre = dx::actor::Location(player);
    for (size_t i = 0; i < sounds.size(); i++) {
        sounds[i].bytes = measureNoise(static_cast<uint32_t>(i + 1));
        double angle = static_cast<double>(i) * 0.7;
        dx::Vector at = centre + dx::Vector { static_cast<float>(1200.0 * std::cos(angle)), static_cast<float>(1200.0 * std::sin(angle)), static_cast<float>(static_cast<double>(i % 5) * 80.0 - 160.0) };
        REQUIRE(dx::audio::playSound(&sounds[i], nullptr, dx::audio::SlotNone, at, 1.0f, 4000.0f, 1.0f, true) != 0);
    }
    dx::audio::update(&world, centre, {}, 0.0f);
    dx::audio::settleAcoustics();
    REQUIRE(dx::audio::voices().size() == 64);
    // Ten seconds of sound in 10 ms callbacks with 50 updates a second, the listener turning.
    std::vector<float> buffer(441 * 2);
    double directMs = 0;
    double reflectionMs = 0;
    int samples = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; i++) {
        if (i % 2 == 0) {
            dx::Rotator turning;
            turning.yaw = i * 50;
            dx::audio::update(&world, centre, turning, 0.02f);
        }
        dx::audio::render(buffer.data(), 441);
        if (i % 50 == 0) {
            dx::audio::AcousticStatus now = dx::audio::acousticStatus();
            directMs += now.simulation.directMilliseconds;
            reflectionMs += now.simulation.reflectionMilliseconds;
            samples++;
        }
    }
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    dx::audio::AcousticStatus end = dx::audio::acousticStatus();
    dx::audio::MixCost cost = dx::audio::mixCost();
    std::printf("64 voices: mixing %.2f ms per second of sound in the raytraced mode; worker: direct simulation %.2f ms a pass (%zu sources), reflections %.2f ms a run (%d rays, %d bounces), %.1f passes a second; offline run took %.2f s for 10 s of sound\n", static_cast<double>(cost.hrtf), directMs / samples, end.simulation.sources, reflectionMs / samples, end.simulation.rays, end.simulation.bounces, end.simulation.passesPerSecond, wall);
    dx::objects::unloadPackage(dx::Name("15_Area51_Page"));
    audio.shutdown();
}

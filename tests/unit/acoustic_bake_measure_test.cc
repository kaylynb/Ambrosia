// Measurements of baking acoustics for the raytraced sound mode in real levels, not run by
// default ("[.bake-measure]"): probes, bake time and baked size, on a few threads as while
// playing and on all but one as a batch bake.

#include "audio/acoustic_bake.h"
#include "test_paths.h"
#include "audio/acoustic_scene.h"
#include "core/objects.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "audio/audio.h"
#include "audio/raytraced.h"
#include "core/config.h"
#include "engine/console.h"
#include "engine/world.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <complex>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

TEST_CASE("baking acoustics in a small, a typical and the largest level", "[.bake-measure][data]")
{
    if (!dx::audio::acoustics::bakingAvailable()) {
        SKIP("Steam Audio's baking is not available");
    }
    dx::engine::initialize(dx::test::gameData());
    const char* only = std::getenv("AMBROSIA_BAKE_MAP");
    const char* threadsWanted = std::getenv("AMBROSIA_BAKE_THREADS");
    std::vector<std::string> maps = { "02_NYC_Bar", "01_NYC_UNATCOHQ", "15_Area51_Page" };
    if (only && std::string(only) == "all") {
        maps.clear();
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(dx::test::gameData()) / "Maps")) {
            if (entry.path().extension() == ".dx") {
                maps.push_back(entry.path().stem().string());
            }
        }
        std::sort(maps.begin(), maps.end());
    } else if (only) {
        maps = { only };
    }
    for (const std::string& name : maps) {
        const char* map = name.c_str();
        dx::objects::unloadPackage(dx::Name(map));
        dx::World world;
        REQUIRE(world.loadMap(map));
        dx::audio::acoustics::Geometry geometry = dx::audio::acoustics::levelGeometry(world.level);
        if (std::getenv("AMBROSIA_PROBES_ONLY")) {
            float used = 0;
            std::vector<dx::audio::acoustics::Probe> capped = dx::audio::acoustics::placeProbes(geometry, {}, &used);
            std::printf("%s: capped, %zu probes %.1f m apart\n", map, capped.size(), static_cast<double>(used));
            dx::audio::acoustics::BakeSettings uncapped;
            uncapped.maxProbes = 0;
            std::vector<dx::audio::acoustics::Probe> probes = dx::audio::acoustics::placeProbes(geometry, uncapped);
            dx::Vector low { 1e9f, 1e9f, 1e9f };
            dx::Vector high { -1e9f, -1e9f, -1e9f };
            for (const auto& probe : probes) {
                const dx::Vector& v = probe.location;
                low = { std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z) };
                high = { std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z) };
            }
            std::vector<dx::Vector> nav;
            for (dx::UObject* actor : world.level->actors) {
                if (actor && actor->data && !actor->destroyed && actor->isA(dx::engineClasses().navigationPoint)) {
                    nav.push_back(dx::actor::Location(actor));
                }
            }
            size_t near8 = 0;
            size_t near5 = 0;
            for (const auto& probe : probes) {
                float best = 1e30f;
                for (const dx::Vector& n : nav) {
                    float dx2 = (probe.location.x - n.x) * (probe.location.x - n.x) + (probe.location.y - n.y) * (probe.location.y - n.y);
                    float dz = std::fabs(probe.location.z - n.z);
                    if (dz < 160.0f) {
                        best = std::min(best, dx2);
                    }
                }
                near8 += best < 420.0f * 420.0f ? 1u : 0u;
                near5 += best < 262.0f * 262.0f ? 1u : 0u;
            }
            std::printf("%s: %zu nav points, %zu probes within 8 m, %zu within 5 m\n", map, nav.size(), near8, near5);
            // Probes a walker could reach: flood fill over neighbouring columns whose floors
            // differ by at most a step (or a jump), from the probe nearest each navigation point.
            {
                float spacing = 2.0f * dx::audio::acoustics::UnitsPerMetre;
                constexpr float Climb = 48.0f;
                std::unordered_map<int64_t, std::vector<size_t>> columns;
                auto keyOf = [&](float x, float y) {
                    int64_t cx = static_cast<int64_t>(std::floor(x / spacing + 0.5f));
                    int64_t cy = static_cast<int64_t>(std::floor(y / spacing + 0.5f));
                    return cx * 1000003 + cy;
                };
                for (size_t i = 0; i < probes.size(); i++) {
                    columns[keyOf(probes[i].location.x, probes[i].location.y)].push_back(i);
                }
                std::vector<bool> reached(probes.size(), false);
                std::vector<size_t> queue;
                for (const dx::Vector& n : nav) {
                    float best = 1e30f;
                    size_t pick = probes.size();
                    for (size_t i = 0; i < probes.size(); i++) {
                        const dx::Vector& p = probes[i].location;
                        float d = (p.x - n.x) * (p.x - n.x) + (p.y - n.y) * (p.y - n.y) + 4.0f * (p.z - n.z) * (p.z - n.z);
                        if (d < best) {
                            best = d;
                            pick = i;
                        }
                    }
                    if (pick < probes.size() && best < 420.0f * 420.0f && !reached[pick]) {
                        reached[pick] = true;
                        queue.push_back(pick);
                    }
                }
                while (!queue.empty()) {
                    size_t i = queue.back();
                    queue.pop_back();
                    const dx::Vector& p = probes[i].location;
                    for (int ox = -1; ox <= 1; ox++) {
                        for (int oy = -1; oy <= 1; oy++) {
                            auto found = columns.find(keyOf(p.x + static_cast<float>(ox) * spacing, p.y + static_cast<float>(oy) * spacing));
                            if (found == columns.end()) {
                                continue;
                            }
                            for (size_t j : found->second) {
                                if (!reached[j] && std::fabs(probes[j].location.z - p.z) <= Climb) {
                                    reached[j] = true;
                                    queue.push_back(j);
                                }
                            }
                        }
                    }
                }
                size_t count = static_cast<size_t>(std::count(reached.begin(), reached.end(), true));
                std::printf("%s: %zu probes reachable\n", map, count);
            }
            std::printf("%s: %zu probes over %.0f %.0f %.0f .. %.0f %.0f %.0f\n", map, probes.size(), static_cast<double>(low.x), static_cast<double>(low.y), static_cast<double>(low.z), static_cast<double>(high.x), static_cast<double>(high.y), static_cast<double>(high.z));
            std::fflush(stdout);
            continue;
        }
        std::vector<int32_t> counts;
        if (threadsWanted) {
            counts.push_back(std::atoi(threadsWanted));
        } else {
            counts = { 2, static_cast<int32_t>(std::thread::hardware_concurrency()) - 1 };
        }
        for (int32_t threads : counts) {
            dx::audio::acoustics::BakeSettings settings;
            settings.threads = threads;
            if (const char* spacing = std::getenv("AMBROSIA_BAKE_SPACING")) {
                settings.spacing = static_cast<float>(std::atof(spacing));
            }
            std::string error;
            auto start = std::chrono::steady_clock::now();
            auto baked = dx::audio::acoustics::bake(geometry, dx::audio::acoustics::defaultMaterials(), settings, {}, &error);
            double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            INFO(error);
            REQUIRE(baked);
            std::printf("%s, %d threads: %zu triangles, %zu probes of %zu placed (radius %.0f units), probes %.1f s, reverb %.1f s, paths %.1f s, total %.1f s; reverb %.1f KB, paths %.1f KB, saved %.1f KB\n", map, threads, geometry.triangles.size(), baked->probes.size(), baked->candidates, baked->probes.empty() ? 0.0 : static_cast<double>(baked->probes[0].radius), baked->probeMilliseconds / 1000.0, baked->reverbMilliseconds / 1000.0, baked->pathMilliseconds / 1000.0, total, static_cast<double>(baked->reverbBytes) / 1024.0, static_cast<double>(baked->pathBytes) / 1024.0, static_cast<double>(baked->batch.size()) / 1024.0);
            if (std::getenv("AMBROSIA_BAKE_PROBES")) {
                for (const auto& probe : baked->probes) {
                    std::printf("  probe %.0f %.0f %.0f open %d\n", static_cast<double>(probe.location.x), static_cast<double>(probe.location.y), static_cast<double>(probe.location.z), geometry.space.open(probe.location) ? 1 : 0);
                }
                dx::Vector low { 1e9f, 1e9f, 1e9f };
                dx::Vector high { -1e9f, -1e9f, -1e9f };
                for (const dx::Vector& v : geometry.vertices) {
                    low = { std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z) };
                    high = { std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z) };
                }
                std::printf("  bounds %.0f %.0f %.0f .. %.0f %.0f %.0f, %zu space nodes\n", static_cast<double>(low.x), static_cast<double>(low.y), static_cast<double>(low.z), static_cast<double>(high.x), static_cast<double>(high.y), static_cast<double>(high.z), geometry.space.nodes.size());
            }
            std::fflush(stdout);
        }
    }
}

namespace {
    // Energy per band of a stereo buffer's mid signal, from 1024-point FFTs with a Hann window:
    // 125-800 Hz, 800 Hz-5 kHz and 5-16 kHz, at 44.1 kHz.
    std::array<double, 3> bandEnergy(const std::vector<float>& stereo)
    {
        constexpr size_t N = 1024;
        std::array<double, 3> bands {};
        std::vector<std::complex<double>> x(N);
        for (size_t start = 0; start + N <= stereo.size() / 2; start += N) {
            for (size_t i = 0; i < N; i++) {
                double w = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * static_cast<double>(i) / static_cast<double>(N));
                x[i] = { 0.5 * (static_cast<double>(stereo[(start + i) * 2]) + static_cast<double>(stereo[(start + i) * 2 + 1])) * w, 0.0 };
            }
            for (size_t i = 1, j = 0; i < N; i++) {
                size_t bit = N >> 1;
                for (; j & bit; bit >>= 1) {
                    j ^= bit;
                }
                j ^= bit;
                if (i < j) {
                    std::swap(x[i], x[j]);
                }
            }
            for (size_t length = 2; length <= N; length <<= 1) {
                double angle = -2.0 * 3.141592653589793 / static_cast<double>(length);
                std::complex<double> step(std::cos(angle), std::sin(angle));
                for (size_t i = 0; i < N; i += length) {
                    std::complex<double> w(1.0, 0.0);
                    for (size_t k = 0; k < length / 2; k++) {
                        std::complex<double> u = x[i + k];
                        std::complex<double> v = x[i + k + length / 2] * w;
                        x[i + k] = u + v;
                        x[i + k + length / 2] = u - v;
                        w *= step;
                    }
                }
            }
            for (size_t k = 1; k < N / 2; k++) {
                double hz = static_cast<double>(k) * 44100.0 / static_cast<double>(N);
                double power = std::norm(x[k]);
                if (hz >= 125 && hz < 800) {
                    bands[0] += power;
                } else if (hz >= 800 && hz < 5000) {
                    bands[1] += power;
                } else if (hz >= 5000 && hz < 16000) {
                    bands[2] += power;
                }
            }
        }
        return bands;
    }

    double decibels(double energy)
    {
        return energy > 0 ? 10.0 * std::log10(energy) : -200.0;
    }
}

// The case that asked for pathing: a klaxon on a camera behind a wall in the cathedral, heard
// in the original mixer, and in the raytraced mode without and with baked paths.
TEST_CASE("the cathedral klaxon behind a wall in the original mode and raytraced with pathing off and on", "[.bake-measure][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::ConfigSnapshot settings;
    dx::test::TempDir userData("klaxon-cache");
    std::string previous = std::getenv("XDG_DATA_HOME") ? std::getenv("XDG_DATA_HOME") : "";
#ifdef _WIN32
    _putenv_s("XDG_DATA_HOME", userData.path().string().c_str());
#else
    setenv("XDG_DATA_HOME", userData.path().string().c_str(), 1);
#endif
    const char* section = "Galaxy.GalaxyAudioSubsystem";
    dx::config::system().set(section, "AmbrosiaRaytracedAudio", "False");
    dx::config::system().set(section, "AmbrosiaHRTF", "False");
    dx::test::OfflineAudio audio;
    dx::objects::unloadPackage(dx::Name("11_Paris_Cathedral"));
    {
        dx::World world;
        REQUIRE(world.loadMap("11_Paris_Cathedral"));
        dx::UObject* player = world.player();
        REQUIRE(player);
        dx::UObject* camera = nullptr;
        for (dx::UObject* actor : world.level->actors) {
            if (actor && actor->name == dx::Name("SecurityCamera0")) {
                camera = actor;
            }
        }
        REQUIRE(camera);
        dx::UObject* klaxon = nullptr;
        for (const char* path : { "Ambient.Klaxon2", "Ambient.Ambient.Klaxon2", "Ambient.Klaxons.Klaxon2", "Ambient.Alarms.Klaxon2" }) {
            if (!klaxon) {
                klaxon = dx::objects::loadObject(path, nullptr);
            }
        }
        REQUIRE(klaxon);
        dx::actor::AmbientSound(camera) = dx::cast<dx::USound>(klaxon);
        dx::console::execute(player, "teleport 3450 -1300 -294 0");
        for (int i = 0; i < 5; i++) {
            world.tick(1.0f / 30.0f);
        }
        dx::Vector ear = dx::actor::Location(player);
        dx::Rotator facing = dx::actor::Rotation(player);
        dx::Vector eye = ear + dx::Vector { 0, 0, dx::pawn::EyeHeight(player) };
        dx::Vector at = dx::actor::Location(camera);
        std::printf("player at %.0f %.0f %.0f (eye %.0f), camera at %.0f %.0f %.0f, line check from the eye %s\n", static_cast<double>(ear.x), static_cast<double>(ear.y), static_cast<double>(ear.z), static_cast<double>(eye.z), static_cast<double>(at.x), static_cast<double>(at.y), static_cast<double>(at.z), world.fastLineCheck(eye, at) ? "clear" : "blocked");
        auto settle = [&](int updates) {
            std::vector<float> scratch(1470 * 2);
            for (int i = 0; i < updates; i++) {
                dx::audio::update(&world, ear, facing, 1.0f / 30.0f);
                dx::audio::render(scratch.data(), 1470);
            }
            dx::audio::settleAcoustics();
        };
        auto klaxonVoice = [&]() {
            dx::audio::VoiceStatus found;
            for (const dx::audio::VoiceStatus& voice : dx::audio::voices()) {
                if (voice.actor == camera) {
                    found = voice;
                }
            }
            return found;
        };
        auto measure = [&](const char* label) -> std::array<double, 4> {
            settle(30);
            dx::audio::VoiceStatus voice = klaxonVoice();
            REQUIRE(voice.id != 0);
            for (const dx::audio::VoiceStatus& other : dx::audio::voices()) {
                dx::audio::setVoiceSoloed(other.id, other.id == voice.id);
            }
            settle(10);
            // Sixteen seconds, whole blasts of the klaxon many times over, updated at 30 Hz as the
            // game would.
            // Whole loops of the klaxon, at least sixteen seconds, so where in its cycle the
            // measurement starts does not matter.
            double loop = voice.step > 0 ? static_cast<double>(voice.frames) / voice.step : 44100.0;
            int loops = std::max(1, static_cast<int>(std::ceil(16.0 * 44100.0 / loop)));
            size_t frames = static_cast<size_t>(std::llround(loop * loops));
            std::vector<float> all;
            std::vector<float> chunk(1470 * 2);
            for (size_t done = 0; done < frames; done += 1470) {
                dx::audio::update(&world, ear, facing, 1.0f / 30.0f);
                size_t count = std::min<size_t>(1470, frames - done);
                dx::audio::render(chunk.data(), count);
                all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(count * 2));
            }
            dx::audio::AcousticStatus acoustic = dx::audio::acousticStatus();
            std::printf("  %d loops of %.2f s; reverb from %s, %.2f %.2f %.2f s; voice level %d step %.5f position %zu id %d\n", loops, loop / 44100.0, acoustic.simulation.reverbBaked ? "baked" : "live", static_cast<double>(acoustic.simulation.reverbTimes[0]), static_cast<double>(acoustic.simulation.reverbTimes[1]), static_cast<double>(acoustic.simulation.reverbTimes[2]), voice.volume, voice.step, voice.position, voice.id);
            voice = klaxonVoice();
            double energy = 0;
            for (float sample : all) {
                energy += static_cast<double>(sample) * static_cast<double>(sample);
            }
            std::array<double, 3> bands = bandEnergy(all);
            std::printf("%s: level %.1f dB; 125-800 Hz %.1f dB, 0.8-5 kHz %.1f dB, 5-16 kHz %.1f dB; distance %.0f, occlusion %.2f, transmission %.3f %.3f %.3f, path %d length %.0f way %.2f %.2f %.2f, pathed %.3f %.3f %.3f\n", label, decibels(energy / static_cast<double>(all.size())), decibels(bands[0]), decibels(bands[1]), decibels(bands[2]), static_cast<double>(voice.distance), static_cast<double>(voice.occlusion), static_cast<double>(voice.transmission[0]), static_cast<double>(voice.transmission[1]), static_cast<double>(voice.transmission[2]), voice.pathFound ? 1 : 0, static_cast<double>(voice.pathLength), static_cast<double>(voice.pathDirection.x), static_cast<double>(voice.pathDirection.y), static_cast<double>(voice.pathDirection.z), static_cast<double>(voice.pathed[0]), static_cast<double>(voice.pathed[1]), static_cast<double>(voice.pathed[2]));
            std::fflush(stdout);
            return { decibels(energy / static_cast<double>(all.size())), decibels(bands[0]), decibels(bands[1]), decibels(bands[2]) };
        };
        auto mode = [&](bool raytraced) {
            dx::config::system().set(section, "AmbrosiaRaytracedAudio", raytraced ? "True" : "False");
            dx::audio::applyMenuSettings();
        };
        auto all = [&](const char* where) {
            std::printf("-- %s\n", where);
            const char* names[3] = { "original (Galaxy stereo)", "raytraced, pathing off", "raytraced, pathing on" };
            std::array<std::vector<std::array<double, 4>>, 3> runs;
            for (int repeat = 0; repeat < 4; repeat++) {
                for (int which = 0; which < 3; which++) {
                    mode(which > 0);
                    dx::audio::setBakedUse({ true, which == 2 });
                    runs[static_cast<size_t>(which)].push_back(measure(names[which]));
                }
            }
            for (int which = 0; which < 3; which++) {
                std::printf("   %-26s", names[which]);
                for (size_t column = 0; column < 4; column++) {
                    double sum = 0;
                    double low = 1e9;
                    double high = -1e9;
                    for (const auto& run : runs[static_cast<size_t>(which)]) {
                        sum += run[column];
                        low = std::min(low, run[column]);
                        high = std::max(high, run[column]);
                    }
                    const char* label[4] = { "level", "125-800 Hz", "0.8-5 kHz", "5-16 kHz" };
                    std::printf(" %s %.1f dB (%.1f to %.1f)", label[column], sum / 4.0, low, high);
                }
                std::printf("\n");
            }
            std::fflush(stdout);
        }; // The bake, in the background on entering the level in the raytraced mode.
        mode(true);
        if (!dx::audio::raytracedActive()) {
            SKIP("Steam Audio's simulation is not available");
        }
        auto start = std::chrono::steady_clock::now();
        while (dx::audio::bakeReport().state != dx::audio::acoustics::BakeState::Ready && std::chrono::steady_clock::now() - start < std::chrono::seconds(180)) {
            settle(3);
        }
        dx::audio::acoustics::BakeReport report = dx::audio::bakeReport();
        std::printf("background bake: %s, %zu probes, %.1f s, %.1f MB\n", dx::audio::acoustics::bakeStateName(report.state), report.probes, report.bakeSeconds, static_cast<double>(report.fileBytes) / 1048576.0);
        REQUIRE(report.state == dx::audio::acoustics::BakeState::Ready);
        all("at 3450 -1300 -294, the spot first reported");
        if (std::getenv("AMBROSIA_KLAXON_ORDER")) {
            mode(false);
            for (int i = 0; i < 3; i++) {
                measure("order check, original");
            }
            mode(true);
            for (bool pathing : { false, true, false, true, true, false }) {
                dx::audio::setBakedUse({ true, pathing });
                measure(pathing ? "order check, pathing on" : "order check, pathing off");
            }
        }
        // A spot truly behind a wall that sound can still get round to: of the probes the BSP line
        // check finds blocked from the camera, nearest first, the first a baked path reaches
        // with the doors as they stand. Some blocked probes are in sealed pockets of open space,
        // which no path reaches.
        std::vector<const dx::audio::acoustics::Probe*> blocked;
        for (const dx::audio::acoustics::Probe& probe : report.data->probes) {
            if ((probe.location - at).size() < 2000.0f && !world.fastLineCheck(probe.location, at)) {
                blocked.push_back(&probe);
            }
        }
        std::sort(blocked.begin(), blocked.end(), [&](const auto* a, const auto* b) {
            return (a->location - at).size() < (b->location - at).size();
        });
        const dx::audio::acoustics::Probe* hidden = nullptr;
        {
            auto level = std::make_shared<dx::audio::acoustics::Geometry>(dx::audio::acoustics::levelGeometry(world.level));
            auto search = dx::audio::acoustics::Simulation::create(44100, 4);
            REQUIRE(search);
            search->setGeometry(level);
            search->setMoverPoses(dx::audio::acoustics::moverPoses(world.level));
            search->setSources({ { 1, at } });
            search->setBaked(report.data);
            size_t tried = 0;
            size_t sealed = 0;
            for (const auto* candidate : blocked) {
                if (tried == 60) {
                    break;
                }
                tried++;
                search->setListener(candidate->location, facing);
                search->settle();
                search->settle();
                std::unordered_map<int32_t, dx::audio::acoustics::PathResult> found;
                uint64_t version = 0;
                search->pathResults(found, version);
                if (found[1].found) {
                    hidden = candidate;
                    break;
                }
                sealed++;
            }
            std::printf("%zu probes within 2000 units blocked from the camera; tried %zu nearest first, %zu with no path\n", blocked.size(), tried, sealed);
        }
        REQUIRE(hidden);
        char command[96];
        std::snprintf(command, sizeof(command), "teleport %.0f %.0f %.0f 0", static_cast<double>(hidden->location.x), static_cast<double>(hidden->location.y), static_cast<double>(hidden->location.z - 40));
        dx::console::execute(player, command);
        for (int i = 0; i < 5; i++) {
            world.tick(1.0f / 30.0f);
        }
        ear = dx::actor::Location(player);
        eye = ear + dx::Vector { 0, 0, dx::pawn::EyeHeight(player) };
        std::printf("hidden spot %.0f %.0f %.0f, %.0f units from the camera, line check from the eye %s\n", static_cast<double>(ear.x), static_cast<double>(ear.y), static_cast<double>(ear.z), static_cast<double>((eye - at).size()), world.fastLineCheck(eye, at) ? "clear" : "blocked");
        all(command);
        // Whether a baked route exists that a mover (a closed door) breaks: the same data with the
        // movers left out, and with them where they stand.
        for (bool withMovers : { false, true }) {
            auto level = std::make_shared<dx::audio::acoustics::Geometry>(dx::audio::acoustics::levelGeometry(world.level));
            auto simulation = dx::audio::acoustics::Simulation::create(44100, 4);
            REQUIRE(simulation);
            if (!withMovers) {
                level->movers.clear();
            }
            simulation->setGeometry(level);
            simulation->setMoverPoses(dx::audio::acoustics::moverPoses(world.level));
            simulation->setListener(eye, facing);
            simulation->setSources({ { 1, at } });
            simulation->setBaked(report.data);
            simulation->setTracedSource(1);
            simulation->settle();
            simulation->settle();
            std::unordered_map<int32_t, dx::audio::acoustics::PathResult> paths;
            uint64_t version = 0;
            simulation->pathResults(paths, version);
            const dx::audio::acoustics::PathResult& path = paths[1];
            size_t broken = 0;
            std::vector<dx::audio::acoustics::PathSegment> legs = simulation->tracedPath();
            for (const auto& leg : legs) {
                broken += leg.occluded ? 1u : 0u;
            }
            std::printf("%s: path %d, length %.0f, eq %.2f %.2f %.2f, %zu legs checked, %zu broken\n", withMovers ? "movers where they stand" : "no movers", path.found ? 1 : 0, static_cast<double>(path.length), static_cast<double>(path.eq[0]), static_cast<double>(path.eq[1]), static_cast<double>(path.eq[2]), legs.size(), broken);
            std::fflush(stdout);
        }
        dx::objects::unloadPackage(dx::Name("11_Paris_Cathedral"));
    }
    audio.shutdown();
#ifdef _WIN32
    _putenv_s("XDG_DATA_HOME", previous.c_str());
#else
    if (previous.empty()) {
        unsetenv("XDG_DATA_HOME");
    } else {
        setenv("XDG_DATA_HOME", previous.c_str(), 1);
    }
#endif
}

// What pathing costs with every channel playing: the mixer's time for each second of sound, and
// the worker's pathing run, without and with it, in 15_Area51_Page once its bake is in use.
TEST_CASE("pathing's cost in 15_Area51_Page with every channel playing", "[.bake-measure][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::ConfigSnapshot settings;
    dx::test::TempDir userData("pathing-cost-cache");
    std::string previous = std::getenv("XDG_DATA_HOME") ? std::getenv("XDG_DATA_HOME") : "";
#ifdef _WIN32
    _putenv_s("XDG_DATA_HOME", userData.path().string().c_str());
#else
    setenv("XDG_DATA_HOME", userData.path().string().c_str(), 1);
#endif
    dx::config::system().set("Galaxy.GalaxyAudioSubsystem", "AmbrosiaRaytracedAudio", "True");
    dx::config::system().set("Galaxy.GalaxyAudioSubsystem", "EffectsChannels", "64");
    dx::test::OfflineAudio audio;
    if (!dx::audio::raytracedActive()) {
        SKIP("Steam Audio's simulation is not available");
    }
    dx::objects::unloadPackage(dx::Name("15_Area51_Page"));
    {
        dx::World world;
        REQUIRE(world.loadMap("15_Area51_Page"));
        dx::UObject* player = world.player();
        REQUIRE(player);
        dx::Vector centre = dx::actor::Location(player);
        std::vector<float> buffer(441 * 2);
        auto start = std::chrono::steady_clock::now();
        while (dx::audio::bakeReport().state != dx::audio::acoustics::BakeState::Ready && std::chrono::steady_clock::now() - start < std::chrono::seconds(180)) {
            dx::audio::update(&world, centre, {}, 0.02f);
            dx::audio::render(buffer.data(), 441);
        }
        REQUIRE(dx::audio::bakeReport().state == dx::audio::acoustics::BakeState::Ready);
        std::vector<dx::USound> sounds(64);
        for (size_t i = 0; i < sounds.size(); i++) {
            std::vector<uint8_t> bytes;
            auto u32 = [&bytes](uint32_t v) {
                for (int b = 0; b < 4; b++) {
                    bytes.push_back(static_cast<uint8_t>(v >> (8 * b)));
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
            uint32_t state = static_cast<uint32_t>(i + 1);
            for (uint32_t s = 0; s < frames; s++) {
                state = state * 1664525u + 1013904223u;
                u16(static_cast<uint16_t>(static_cast<int16_t>((static_cast<int32_t>(state >> 16) - 32768) / 2)));
            }
            bytes.insert(bytes.end(), { 's', 'm', 'p', 'l', 60, 0, 0, 0 });
            for (uint32_t s = 0; s < 15; s++) {
                u32(s == 7 ? 1u : (s == 12 ? frames - 1 : 0u));
            }
            sounds[i].bytes = std::move(bytes);
            double angle = static_cast<double>(i) * 0.7;
            dx::Vector at = centre + dx::Vector { static_cast<float>(1200.0 * std::cos(angle)), static_cast<float>(1200.0 * std::sin(angle)), static_cast<float>(static_cast<double>(i % 5) * 80.0 - 160.0) };
            REQUIRE(dx::audio::playSound(&sounds[i], nullptr, dx::audio::SlotNone, at, 1.0f, 4000.0f, 1.0f, true) != 0);
        }
        for (bool pathing : { false, true }) {
            dx::audio::setBakedUse({ true, pathing });
            dx::audio::update(&world, centre, {}, 0.0f);
            dx::audio::settleAcoustics();
            // Ten seconds of sound in 10 ms callbacks with 50 updates a second, the listener turning.
            double pathMs = 0;
            size_t pathed = 0;
            int samples = 0;
            for (int i = 0; i < 1000; i++) {
                if (i % 2 == 0) {
                    dx::Rotator turning;
                    turning.yaw = i * 50;
                    dx::audio::update(&world, centre, turning, 0.02f);
                }
                dx::audio::render(buffer.data(), 441);
                if (i % 50 == 49) {
                    dx::audio::AcousticStatus now = dx::audio::acousticStatus();
                    pathMs += now.simulation.pathMilliseconds;
                    pathed += now.simulation.pathed;
                    samples++;
                }
            }
            size_t adding = 0;
            for (const dx::audio::VoiceStatus& voice : dx::audio::voices()) {
                adding += std::max({ voice.pathed[0], voice.pathed[1], voice.pathed[2] }) > 0 ? 1u : 0u;
            }
            dx::audio::MixCost cost = dx::audio::mixCost();
            dx::audio::AcousticStatus end = dx::audio::acousticStatus();
            std::printf("pathing %s, 64 voices: mixing %.2f ms per second of sound; pathing run %.3f ms a pass on the worker (%.1f passes a second), a path for %.1f sources on average, %zu voices with pathed sound added\n", pathing ? "on" : "off", static_cast<double>(cost.hrtf), pathMs / samples, end.simulation.passesPerSecond, static_cast<double>(pathed) / samples, adding);
            std::fflush(stdout);
        }
        dx::objects::unloadPackage(dx::Name("15_Area51_Page"));
    }
    audio.shutdown();
#ifdef _WIN32
    _putenv_s("XDG_DATA_HOME", previous.c_str());
#else
    if (previous.empty()) {
        unsetenv("XDG_DATA_HOME");
    } else {
        setenv("XDG_DATA_HOME", previous.c_str(), 1);
    }
#endif
}

// What the overlay's path drawing does in play: each voice's path traced in turn, in the
// cathedral at a spot behind a wall, once its bake is in use.
TEST_CASE("tracing each voice's path in the cathedral", "[.bake-measure][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::ConfigSnapshot settings;
    dx::test::TempDir userData("trace-cache");
    std::string previous = std::getenv("XDG_DATA_HOME") ? std::getenv("XDG_DATA_HOME") : "";
#ifdef _WIN32
    _putenv_s("XDG_DATA_HOME", userData.path().string().c_str());
#else
    setenv("XDG_DATA_HOME", userData.path().string().c_str(), 1);
#endif
    dx::config::system().set("Galaxy.GalaxyAudioSubsystem", "AmbrosiaRaytracedAudio", "True");
    dx::test::OfflineAudio audio;
    if (!dx::audio::raytracedActive()) {
        SKIP("Steam Audio's simulation is not available");
    }
    dx::objects::unloadPackage(dx::Name("11_Paris_Cathedral"));
    {
        dx::World world;
        REQUIRE(world.loadMap("11_Paris_Cathedral"));
        dx::UObject* player = world.player();
        dx::console::execute(player, "teleport 3530 -1467 -313 0");
        std::vector<float> buffer(1470 * 2);
        auto step = [&] {
            world.tick(1.0f / 30.0f);
            dx::audio::update(&world, dx::actor::Location(player), dx::actor::Rotation(player), 1.0f / 30.0f);
            dx::audio::render(buffer.data(), 1470);
        };
        auto start = std::chrono::steady_clock::now();
        while (dx::audio::bakeReport().state != dx::audio::acoustics::BakeState::Ready && std::chrono::steady_clock::now() - start < std::chrono::seconds(180)) {
            step();
        }
        REQUIRE(dx::audio::bakeReport().state == dx::audio::acoustics::BakeState::Ready);
        std::printf("baked; tracing voices\n");
        std::fflush(stdout);
        for (int round = 0; round < 3; round++) {
            for (const dx::audio::VoiceStatus& voice : dx::audio::voices()) {
                std::printf("  tracing %d %s at %.0f %.0f %.0f\n", voice.id, voice.sound ? voice.sound->name.str().c_str() : "", static_cast<double>(voice.location.x), static_cast<double>(voice.location.y), static_cast<double>(voice.location.z));
                std::fflush(stdout);
                dx::audio::setTracedVoice(voice.id);
                for (int i = 0; i < 10; i++) {
                    step();
                }
                dx::audio::settleAcoustics();
            }
        }
        dx::audio::setTracedVoice(0);
        std::printf("done\n");
        dx::objects::unloadPackage(dx::Name("11_Paris_Cathedral"));
    }
    audio.shutdown();
#ifdef _WIN32
    _putenv_s("XDG_DATA_HOME", previous.c_str());
#else
    if (previous.empty()) {
        unsetenv("XDG_DATA_HOME");
    } else {
        setenv("XDG_DATA_HOME", previous.c_str(), 1);
    }
#endif
}

// Leaving a level, or the game, while its bake runs: cancelled at each stage.
TEST_CASE("cancelling a bake at each stage", "[.bake-measure][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::TempDir userData("cancel-cache");
#ifdef _WIN32
    _putenv_s("XDG_DATA_HOME", userData.path().string().c_str());
#else
    setenv("XDG_DATA_HOME", userData.path().string().c_str(), 1);
#endif
    dx::objects::unloadPackage(dx::Name("11_Paris_Cathedral"));
    dx::World world;
    REQUIRE(world.loadMap("11_Paris_Cathedral"));
    auto geometry = std::make_shared<const dx::audio::acoustics::Geometry>(dx::audio::acoustics::levelGeometry(world.level));
    for (auto stage : { dx::audio::acoustics::BakeStage::Reverb, dx::audio::acoustics::BakeStage::Pathing }) {
        for (float at : { 0.0f, 0.3f, 0.8f }) {
            auto baker = std::make_unique<dx::audio::acoustics::Baker>();
            dx::audio::acoustics::BakeSettings settings;
            settings.threads = 2;
            baker->setLevel("11_Paris_Cathedral", geometry, dx::audio::acoustics::defaultMaterials(), settings, true);
            auto start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - start < std::chrono::seconds(60)) {
                dx::audio::acoustics::BakeReport report = baker->report();
                if (report.state == dx::audio::acoustics::BakeState::Ready || (report.state == dx::audio::acoustics::BakeState::Baking && report.stage == stage && report.progress >= at)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            dx::audio::acoustics::BakeReport report = baker->report();
            std::printf("cancelling while %s at %.0f%% (%s)\n", dx::audio::acoustics::bakeStageName(report.stage), static_cast<double>(report.progress * 100), dx::audio::acoustics::bakeStateName(report.state));
            std::fflush(stdout);
            baker.reset();
            std::printf("  cancelled\n");
            std::fflush(stdout);
        }
    }
    dx::objects::unloadPackage(dx::Name("11_Paris_Cathedral"));
}
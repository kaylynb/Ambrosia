#include "audio/raytraced_mode.h"

#include "audio/audio.h"
#include "audio/raytraced.h"
#include "core/log.h"
#include "core/objects.h"
#include "core/settings.h"
#include "core/workers.h"
#include "engine/resources.h"
#include "engine/world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace dx::audio {
    namespace {
        // Threads a level is baked on in the background while it is played: few enough to leave
        // the game its cores (15_Area51_Page, the largest, then takes about 30 s on a 7950X).
        constexpr int32_t BackgroundBakeThreads = 2;

        // The pathed sound: per band, what comes round by the baked path beyond what the direct
        // sound already brings. The path's amplitude is its loss from bending round edges times
        // Galaxy's distance curve along the path over the curve along the straight line, which
        // the voice's level already holds; the pathed sound adds only the energy the path has
        // over the direct sound (occluded and transmitted). So a source in plain view, whose
        // path is the straight line, gains nothing, a source with no path gains nothing, and a
        // hidden one is heard about as loud as its path, from the way it arrives.
        std::array<float, 3> pathedAmplitude(const RaytracedVoice& voice, float voiceDistance, float radius)
        {
            std::array<float, 3> amplitude {};
            if (!voice.path.found || !voice.simulated || radius <= 0) {
                return amplitude;
            }
            float straight = std::clamp(1.0f - voiceDistance / radius, 0.0f, 1.0f);
            float along = std::clamp(1.0f - voice.path.length / radius, 0.0f, 1.0f);
            if (straight <= 0) {
                return amplitude;
            }
            float distance = std::min(along / straight, 1.0f);
            for (size_t band = 0; band < 3; band++) {
                float pathed = voice.path.amplitude[band] * distance;
                float direct = voice.direct.occlusion + (1.0f - voice.direct.occlusion) * voice.direct.transmission[band];
                amplitude[band] = std::sqrt(std::max(0.0f, pathed * pathed - direct * direct));
            }
            return amplitude;
        }
    }

    void RaytracedMode::readSettings()
    {
        // On top of the HRTF, which it turns on itself, occlusion and reverb simulated by Steam
        // Audio from the level's geometry.
        raytracedWanted = settings::get(settings::raytracedAudio);
        reverbLevel = settings::get(settings::reverbLevel);
        // The baked acoustics: baking a level in the background when its cache has none, and
        // using the baked reverb and the baked paths where there are some.
        backgroundBake = settings::get(settings::bakeAcoustics);
        bakedUse.reverb = settings::get(settings::bakedReverb);
        bakedUse.pathing = settings::get(settings::pathing);
        if (simulation) {
            simulation->setBakedUse(bakedUse);
        }
    }

    std::unique_ptr<acoustics::Simulation> RaytracedMode::makeSimulation(uint32_t rate, size_t channels)
    {
        if (raytracedUnavailable) {
            return nullptr;
        }
        std::unique_ptr<acoustics::Simulation> made = acoustics::Simulation::create(rate, channels);
        if (!made) {
            raytracedUnavailable = true;
            log::warn("DevAudio", "AmbrosiaRaytracedAudio: unavailable, so the HRTF mode plays without it");
        }
        return made;
    }

    void RaytracedMode::install(std::unique_ptr<acoustics::Simulation> made)
    {
        simulation = std::move(made);
        raytracing = nullptr;
        acousticLevel = nullptr;
        acousticLevelGone = false;
        simulation->setPaused(true);
        simulation->setMaterials(materials);
        simulation->setReflectionSettings(reflectionSettings);
        simulation->setBakedUse(bakedUse);
        simulation->setTracedSource(tracedVoice);
    }

    bool RaytracedMode::switchTo(bool on)
    {
        acoustics::Simulation* wanted = on ? simulation.get() : nullptr;
        if (wanted == raytracing) {
            return false;
        }
        raytracing = wanted;
        if (simulation) {
            simulation->setPaused(!wanted);
        }
        if (raytracing) {
            raytracing->filters().renew();
            raytracing->pathFilters().renew();
            raytracing->reverb().reset();
        }
        directPaths.clear();
        directVersion = 0;
        pathResults.clear();
        pathVersion = 0;
        reverbTimes = {};
        return true;
    }

    void RaytracedMode::clearActive()
    {
        raytracing = nullptr;
    }

    void RaytracedMode::dropSimulation()
    {
        raytracing = nullptr;
        simulation.reset();
    }

    void RaytracedMode::shutdown()
    {
        raytracing = nullptr;
        baker.reset();
        simulation.reset();
        acousticLevel = nullptr;
        acousticLevelGone = false;
        acousticMap.clear();
    }

    void RaytracedMode::followLevel(World* world)
    {
        // The scene follows the level: exported here when a level is entered (or the mode is
        // switched on in a level the scene is not of), and built and freed by the simulation's
        // worker. While the mode is off the scene is kept for switching back, unless the level
        // changes, when it is let go of.
        const ULevel* level = world ? world->level : nullptr;
        bool changed = level != acousticLevel || acousticLevelGone;
        if (simulation && !raytracing && acousticLevel && changed) {
            simulation->setGeometry(nullptr);
            simulation->setBaked(nullptr);
            acousticLevel = nullptr;
            acousticLevelGone = false;
            if (baker) {
                baker->setLevel({}, nullptr, materials, {}, false);
            }
        }
        if (!raytracing) {
            return;
        }
        if (level != acousticLevel || acousticLevelGone) {
            acousticLevel = level;
            acousticLevelGone = false;
            auto start = std::chrono::steady_clock::now();
            std::shared_ptr<const acoustics::Geometry> geometry = level ? std::make_shared<const acoustics::Geometry>(acoustics::levelGeometry(world->level)) : nullptr;
            exportMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            raytracing->setGeometry(geometry);
            raytracing->setBaked(nullptr);
            moverPoses.clear();
            // The level's baked data, from the cache or baked in the background; until it is
            // ready the mode runs on its live simulation alone.
            acousticMap = level ? world->level->outermost()->name.str() : std::string();
            if (simulation->canUseBaked()) {
                if (!baker) {
                    baker = std::make_unique<acoustics::Baker>();
                }
                acoustics::BakeSettings settings;
                settings.threads = std::min(BackgroundBakeThreads, workers::threadCap());
                baker->setLevel(acousticMap, level ? geometry : nullptr, materials, settings, backgroundBake);
            }
        }
        if (baker) {
            bool dropped = false;
            if (std::shared_ptr<const acoustics::BakedData> baked = baker->take(dropped)) {
                raytracing->setBaked(std::move(baked));
            } else if (dropped) {
                raytracing->setBaked(nullptr);
            }
        }
        // Movers' places, handed on when any has moved.
        if (world && world->level) {
            std::vector<acoustics::MoverPose> poses = acoustics::moverPoses(world->level);
            if (poses != moverPoses) {
                moverPoses = poses;
                raytracing->setMoverPoses(std::move(poses));
            }
        }
    }

    void RaytracedMode::levelUnloading(const ULevel* level)
    {
        if (level && level == acousticLevel) {
            acousticLevelGone = true;
        }
    }

    void RaytracedMode::hear(const Vector& ear, const Rotator& rotation, std::vector<acoustics::SourceInput> sources)
    {
        raytracing->setListener(ear, rotation);
        raytracing->setSources(std::move(sources));
    }

    void RaytracedMode::allocateBlocks()
    {
        pathMono.assign(steam::BlockFrames, 0.0f);
        pathLeft.assign(steam::BlockFrames, 0.0f);
        pathRight.assign(steam::BlockFrames, 0.0f);
    }

    bool RaytracedMode::beginBlock(const Coords& listenerAxes)
    {
        axes = listenerAxes;
        directNew = raytracing && raytracing->directPaths(directPaths, directVersion);
        pathsNew = raytracing && raytracing->pathResults(pathResults, pathVersion);
        return directNew || pathsNew;
    }

    void RaytracedMode::takeResults(int32_t id, RaytracedVoice& voice) const
    {
        if (directNew) {
            auto found = directPaths.find(id);
            if (found != directPaths.end()) {
                voice.direct = found->second;
                voice.simulated = true;
            }
        }
        // A voice with no baked path has no pathed sound.
        if (pathsNew) {
            auto found = pathResults.find(id);
            voice.path = found != pathResults.end() ? found->second : acoustics::PathResult {};
        }
    }

    bool RaytracedMode::pathing(bool talk) const
    {
        return raytracing && !talk && raytracing->canUseBaked();
    }

    void RaytracedMode::resetChannel(size_t index, RaytracedChannel& channel, bool talk)
    {
        if (raytracing) {
            raytracing->filters().reset(index);
        }
        if (pathing(talk)) {
            raytracing->pathFilters().reset(index);
        }
        channel.pathOn = false;
        channel.pathTail = false;
    }

    void RaytracedMode::filterDirect(size_t index, float* mono, const RaytracedVoice& voice, bool talk)
    {
        if (pathing(talk)) {
            std::copy(mono, mono + steam::BlockFrames, pathMono.begin());
        }
        // Occlusion and transmission before the HRTF. Conversation is never occluded, as Galaxy
        // never obstructs the talk slot.
        if (raytracing && !talk) {
            raytracing->filters().apply(index, mono, voice.direct);
        }
    }

    // The pathed sound of a voice over one block, from pathMono (the voice before its direct
    // filter). It fades in over a block when it starts and out over a block when it stops,
    // keeping its last way and amplitudes; its tail rings out in mixTail.
    void RaytracedMode::mixPath(size_t index, RaytracedVoice& voice, RaytracedChannel& channel, const PathedVoice& from, bool talk, float* out, float* send)
    {
        if (!pathing(talk)) {
            return;
        }
        std::array<float, 3> amplitude = pathedAmplitude(voice, from.distance, from.radius);
        voice.pathed = amplitude;
        bool wanted = std::max({ amplitude[0], amplitude[1], amplitude[2] }) > 1e-4f;
        if (!wanted && !channel.pathOn) {
            return;
        }
        if (wanted) {
            const Vector& way = voice.path.direction;
            float ahead = way.dot(axes.xAxis);
            float right = way.dot(axes.yAxis);
            float up = way.dot(axes.zAxis);
            float length = std::sqrt(ahead * ahead + right * right + up * up);
            channel.pathDirection = length > 1e-6f ? steam::Direction { right / length, up / length, -ahead / length } : from.direction;
            channel.pathBlend = from.spread * voice.path.focus;
            channel.pathAmplitude = amplitude;
        }
        if (wanted != channel.pathOn) {
            float start = channel.pathOn ? 1.0f : 0.0f;
            float end = wanted ? 1.0f : 0.0f;
            for (size_t i = 0; i < steam::BlockFrames; i++) {
                pathMono[i] *= start + (end - start) * static_cast<float>(i) / static_cast<float>(steam::BlockFrames);
            }
        }
        raytracing->pathFilters().apply(index, pathMono.data(), channel.pathAmplitude, pathLeft.data(), pathRight.data(), channel.pathDirection, channel.pathBlend);
        float loudness = steam::Binaural::loudness(channel.pathBlend);
        channel.pathDry = from.dry * loudness;
        channel.pathWet = from.wet * loudness;
        for (size_t i = 0; i < steam::BlockFrames; i++) {
            out[i * 2] += pathLeft[i] * channel.pathDry;
            out[i * 2 + 1] += pathRight[i] * channel.pathDry;
            send[i * 2] += pathLeft[i] * channel.pathWet;
            send[i * 2 + 1] += pathRight[i] * channel.pathWet;
        }
        channel.pathOn = wanted;
        channel.pathMixed = true;
        channel.pathTail = true;
    }

    void RaytracedMode::mixTail(size_t index, RaytracedChannel& channel, float* out, float* send)
    {
        if (channel.pathTail && !channel.pathMixed) {
            channel.pathOn = false;
            channel.pathTail = raytracing && raytracing->canUseBaked() && raytracing->pathFilters().tail(index, pathLeft.data(), pathRight.data());
            if (channel.pathTail) {
                for (size_t i = 0; i < steam::BlockFrames; i++) {
                    out[i * 2] += pathLeft[i] * channel.pathDry;
                    out[i * 2 + 1] += pathRight[i] * channel.pathDry;
                    send[i * 2] += pathLeft[i] * channel.pathWet;
                    send[i * 2 + 1] += pathRight[i] * channel.pathWet;
                }
            }
        }
        channel.pathMixed = false;
    }

    bool RaytracedMode::mixReverb(const float* send, float* out, bool binaural, bool useReverb)
    {
        // The reverb simulated around the listener takes the place of the zone's unless the
        // debug overlay asks for the zone's to compare.
        if (!raytracing || !binaural || authoredReverb) {
            return false;
        }
        raytracing->reverbTimes(reverbTimes);
        if (useReverb) {
            raytracing->reverb().process(send, out, reverbTimes, reverbLevel);
        }
        return true;
    }

    void RaytracedMode::report(const RaytracedVoice& voice, bool positioned, VoiceStatus& status) const
    {
        status.simulated = raytracing != nullptr && voice.simulated && positioned;
        status.occlusion = voice.direct.occlusion;
        status.transmission = voice.direct.transmission;
        status.pathFound = status.simulated && voice.path.found;
        status.pathLength = voice.path.length;
        status.pathDirection = voice.path.direction;
        status.pathed = voice.pathed;
    }

    bool raytracedActive()
    {
        std::lock_guard<dx::profile::Mutex> lock(mixerMutex());
        return raytracedMode().on();
    }

    std::shared_ptr<const acoustics::Geometry> acousticGeometry()
    {
        RaytracedMode& r = raytracedMode();
        return r.simulation ? r.simulation->geometry() : nullptr;
    }

    AcousticStatus acousticStatus()
    {
        RaytracedMode& r = raytracedMode();
        AcousticStatus status;
        status.built = DX_STEAM_AUDIO != 0;
        status.active = r.raytracing != nullptr;
        status.unavailable = r.raytracedUnavailable;
        status.exportMilliseconds = r.exportMilliseconds;
        if (r.simulation) {
            status.simulation = r.simulation->stats();
        }
        return status;
    }

    const std::vector<acoustics::Material>& acousticMaterials()
    {
        return raytracedMode().materials;
    }

    void setAcousticMaterials(const std::vector<acoustics::Material>& materials)
    {
        RaytracedMode& r = raytracedMode();
        if (materials.size() != r.materials.size()) {
            return;
        }
        r.materials = materials;
        if (r.simulation) {
            r.simulation->setMaterials(r.materials);
        }
        if (r.baker) {
            r.baker->setMaterials(r.materials);
        }
    }

    const std::vector<acoustics::MoverPose>& acousticMoverPoses()
    {
        return raytracedMode().moverPoses;
    }

    void setAcousticMoverPoses(std::vector<acoustics::MoverPose> poses)
    {
        RaytracedMode& r = raytracedMode();
        r.moverPoses = poses;
        if (r.simulation) {
            r.simulation->setMoverPoses(std::move(poses));
        }
    }

    ReverbControls reverbControls()
    {
        RaytracedMode& r = raytracedMode();
        std::lock_guard<dx::profile::Mutex> lock(mixerMutex());
        ReverbControls controls;
        controls.level = r.reverbLevel;
        controls.authored = r.authoredReverb;
        controls.reflections = r.reflectionSettings;
        controls.times = r.reverbTimes;
        return controls;
    }

    void setReverbControls(const ReverbControls& controls)
    {
        RaytracedMode& r = raytracedMode();
        {
            std::lock_guard<dx::profile::Mutex> lock(mixerMutex());
            r.reverbLevel = std::max(0.0f, controls.level);
            r.authoredReverb = controls.authored;
            r.reflectionSettings = controls.reflections;
        }
        if (r.simulation) {
            r.simulation->setReflectionSettings(controls.reflections);
        }
    }

    void setAcousticGeometry(std::shared_ptr<const acoustics::Geometry> geometry)
    {
        RaytracedMode& r = raytracedMode();
        if (r.simulation) {
            r.simulation->setGeometry(std::move(geometry));
        }
    }

    acoustics::BakeReport bakeReport()
    {
        RaytracedMode& r = raytracedMode();
        if (!r.baker) {
            acoustics::BakeReport report;
            report.map = r.acousticMap;
            if (r.simulation && !r.simulation->canUseBaked()) {
                report.state = acoustics::BakeState::Unavailable;
                report.reason = "Steam Audio's baking is not available";
            }
            return report;
        }
        return r.baker->report();
    }

    void rebakeAcoustics()
    {
        RaytracedMode& r = raytracedMode();
        if (r.baker) {
            r.baker->rebake();
        }
    }

    acoustics::BakedUse bakedUse()
    {
        return raytracedMode().bakedUse;
    }

    void setBakedUse(const acoustics::BakedUse& use)
    {
        RaytracedMode& r = raytracedMode();
        r.bakedUse = use;
        if (r.simulation) {
            r.simulation->setBakedUse(use);
        }
    }

    void setTracedVoice(int32_t id)
    {
        RaytracedMode& r = raytracedMode();
        r.tracedVoice = id;
        if (r.simulation) {
            r.simulation->setTracedSource(id);
        }
    }

    std::vector<acoustics::PathSegment> tracedPath()
    {
        RaytracedMode& r = raytracedMode();
        return r.simulation ? r.simulation->tracedPath() : std::vector<acoustics::PathSegment>();
    }

    void setAcousticBaked(std::shared_ptr<const acoustics::BakedData> data)
    {
        RaytracedMode& r = raytracedMode();
        if (r.simulation) {
            r.simulation->setBaked(std::move(data));
        }
    }

    void settleAcoustics()
    {
        RaytracedMode& r = raytracedMode();
        if (r.raytracing) {
            r.raytracing->settle();
        }
    }
}

#include "audio/acoustics.h"

#include "audio/steam_audio.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/workers.h"

#if DX_STEAM_AUDIO
#include "audio/steam_api.h"
#include "audio/steam_handles.h"
#include "audio/steam_scene.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace dx::audio::acoustics {
#if DX_STEAM_AUDIO
    namespace {
        // Sources are spheres this wide for volumetric occlusion, so that a sound placed inside
        // its fitting (a hum in a light, a camera's whirr) is not taken as walled in.
        constexpr float OcclusionRadius = 0.5f;
        constexpr int32_t OcclusionSamples = 16;
        // Surfaces between source and listener whose transmission counts.
        constexpr int32_t TransmissionSurfaces = 3;
        constexpr auto PassInterval = std::chrono::milliseconds(33);
        // Reflections around the listener are simulated about ten times a second.
        constexpr auto ReflectionInterval = std::chrono::milliseconds(100);
        // The longest decay the reflection simulation measures, and the reverb's IR length.
        constexpr float ReverbDuration = 2.0f;
        // Looking for paths again where a mover breaks a baked one: probes see each other as the
        // bake decides (BakeSettings).
        constexpr float PathVisRadius = 0.5f;
        constexpr float PathVisThreshold = 0.1f;
        constexpr float PathVisRange = 25.0f;
        // The live scene checks a baked path's legs with this many samples a probe.
        constexpr int32_t PathVisSamples = 4;
        // Pathing runs about ten times a second, for the most audible hidden sources only.
        constexpr auto PathingInterval = std::chrono::milliseconds(100);
        constexpr size_t MaxPathedSources = 16;

        IPLMaterial steamMaterial(const Material& material)
        {
            return steamscene::material(material);
        }

        IPLVector3 steamPoint(const Vector& unreal)
        {
            return steamscene::point(unreal);
        }

        IPLVector3 steamDirection(const Vector& unreal)
        {
            return { unreal.x, unreal.z, unreal.y };
        }

        // A place and Unreal rotation as Steam Audio's coordinate space. With Unreal's view axes
        // x ahead, y right and z up converted by toSteam, right x up is -ahead, as Steam Audio's
        // right-handed space needs.
        IPLCoordinateSpace3 steamSpace(const Vector& location, const Rotator& rotation)
        {
            Coords axes = rotationAxes(rotation);
            IPLCoordinateSpace3 space {};
            space.ahead = steamDirection(axes.xAxis);
            space.right = steamDirection(axes.yAxis);
            space.up = steamDirection(axes.zAxis);
            space.origin = steamPoint(location);
            return space;
        }
    }

    namespace {
        // A mono direct effect, or none when Steam Audio refuses it.
        steam::DirectEffect makeDirectEffect(const steam::Api* api, const IPLAudioSettings& audio)
        {
            IPLAudioSettings audioCopy = audio;
            IPLDirectEffectSettings settings {};
            settings.numChannels = 1;
            IPLDirectEffect effect = nullptr;
            if (api->iplDirectEffectCreate(api->context, &audioCopy, &settings, &effect) != IPL_STATUS_SUCCESS) {
                effect = nullptr;
            }
            return steam::DirectEffect(effect, { api });
        }

        // Swaps in a fresh effect, which forgets the old one's filter state, or resets the old one
        // when Steam Audio refuses a fresh one.
        void renewDirectEffect(const steam::Api* api, const IPLAudioSettings& audio, steam::DirectEffect& effect)
        {
            if (steam::DirectEffect fresh = makeDirectEffect(api, audio)) {
                effect = std::move(fresh);
            } else {
                api->iplDirectEffectReset(effect.get());
            }
        }
    }

    struct DirectFilters::Impl {
        const steam::Api* api = nullptr;
        IPLAudioSettings audio {};
        std::vector<steam::DirectEffect> effects;
    };

    void DirectFilters::apply(size_t channel, float* mono, const DirectPath& path)
    {
        IPLAudioBuffer buffer { 1, static_cast<IPLint32>(steam::BlockFrames), &mono };
        IPLDirectEffectParams params {};
        params.flags = static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
        params.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
        params.distanceAttenuation = 1;
        params.airAbsorption[0] = params.airAbsorption[1] = params.airAbsorption[2] = 1;
        params.directivity = 1;
        params.occlusion = path.occlusion;
        for (size_t band = 0; band < 3; band++) {
            params.transmission[band] = path.transmission[band];
        }
        impl->api->iplDirectEffectApply(impl->effects[channel].get(), &params, &buffer, &buffer);
    }

    void DirectFilters::reset(size_t channel)
    {
        impl->api->iplDirectEffectReset(impl->effects[channel].get());
    }

    void DirectFilters::renew()
    {
        for (steam::DirectEffect& effect : impl->effects) {
            renewDirectEffect(impl->api, impl->audio, effect);
        }
    }

    struct PathFilters::Impl {
        const steam::Api* api = nullptr;
        IPLAudioSettings audio {};
        std::vector<steam::DirectEffect> effects;
        std::unique_ptr<steam::Binaural> binaural;
    };

    void PathFilters::apply(size_t channel, float* mono, const std::array<float, 3>& amplitude, float* left, float* right, steam::Direction direction, float blend)
    {
        // A direct effect with everything occluded and the band gains as transmission is a
        // three-band equaliser.
        IPLAudioBuffer buffer { 1, static_cast<IPLint32>(steam::BlockFrames), &mono };
        IPLDirectEffectParams params {};
        params.flags = static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
        params.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
        params.distanceAttenuation = 1;
        params.airAbsorption[0] = params.airAbsorption[1] = params.airAbsorption[2] = 1;
        params.directivity = 1;
        params.occlusion = 0;
        for (size_t band = 0; band < 3; band++) {
            params.transmission[band] = std::clamp(amplitude[band], 0.0f, 1.0f);
        }
        impl->api->iplDirectEffectApply(impl->effects[channel].get(), &params, &buffer, &buffer);
        impl->binaural->apply(channel, mono, left, right, direction, blend);
    }

    bool PathFilters::tail(size_t channel, float* left, float* right)
    {
        return impl->binaural->tail(channel, left, right);
    }

    void PathFilters::reset(size_t channel)
    {
        impl->api->iplDirectEffectReset(impl->effects[channel].get());
        impl->binaural->reset(channel);
    }

    void PathFilters::renew()
    {
        for (size_t channel = 0; channel < impl->effects.size(); channel++) {
            renewDirectEffect(impl->api, impl->audio, impl->effects[channel]);
            impl->binaural->reset(channel);
        }
    }

    struct ListenerReverb::Impl {
        const steam::Api* api = nullptr;
        std::array<steam::ReflectionEffect, 2> effects;
        IPLint32 irSize = 0;
        std::array<std::vector<float>, 2> in;
        std::array<std::vector<float>, 2> out;
    };

    void ListenerReverb::process(const float* send, float* stereo, const std::array<float, 3>& times, float level)
    {
        if (times[0] <= 0 && times[1] <= 0 && times[2] <= 0) {
            return;
        }
        IPLReflectionEffectParams params {};
        params.type = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
        for (size_t band = 0; band < 3; band++) {
            params.reverbTimes[band] = times[band];
        }
        params.numChannels = 1;
        params.irSize = impl->irSize;
        for (size_t ear = 0; ear < 2; ear++) {
            float* in = impl->in[ear].data();
            float* out = impl->out[ear].data();
            for (size_t i = 0; i < steam::BlockFrames; i++) {
                in[i] = send[i * 2 + ear];
            }
            IPLAudioBuffer input { 1, static_cast<IPLint32>(steam::BlockFrames), &in };
            IPLAudioBuffer output { 1, static_cast<IPLint32>(steam::BlockFrames), &out };
            impl->api->iplReflectionEffectApply(impl->effects[ear].get(), &params, &input, &output, nullptr);
            for (size_t i = 0; i < steam::BlockFrames; i++) {
                stereo[i * 2 + ear] += out[i] * level;
            }
        }
    }

    void ListenerReverb::reset()
    {
        for (const steam::ReflectionEffect& effect : impl->effects) {
            impl->api->iplReflectionEffectReset(effect.get());
        }
    }

    struct Simulation::Impl {
        const steam::Api* api = nullptr;
        // The Steam Audio objects are declared before what is made with them or held in them, so
        // that they go after them: the device before the scenes, the simulator before its sources
        // and probe batch (steam_handles.h, steam_scene.h).
        steam::EmbreeDevice embree;
        IPLSceneType sceneType = IPL_SCENETYPE_DEFAULT;
        steam::Simulator simulator;
        uint32_t rate = 44100;
        std::unique_ptr<DirectFilters> filters;
        std::unique_ptr<PathFilters> pathing;
        std::unique_ptr<ListenerReverb> reverb;
        // Whether the library can bake and path, so sources are made able to.
        bool canBake = false;

        // Worker-owned Steam Audio objects.
        steam::Source listenerSource;
        steam::SimulatorBatch batch;
        size_t batchProbes = 0;
        std::string bakeError;
        std::chrono::steady_clock::time_point lastReflections {};
        std::chrono::steady_clock::time_point lastPathing {};
        // The level's scene, declared before what is in it so that it goes after them.
        steamscene::Scene scene;
        steamscene::StaticMesh staticMesh;
        std::unordered_map<int32_t, steam::Source> sources;

        // Each mover: its shape as a scene of its own, placed in the level's scene by an instance.
        struct MoverInstance {
            steamscene::Scene scene;
            steamscene::StaticMesh mesh;
            steamscene::InstancedMesh instance;
            MoverPose pose;
        };

        std::unordered_map<int32_t, MoverInstance> movers;
        std::vector<MoverPose> appliedPoses;
        // The table the scene was built or last updated with, on the worker.
        std::vector<Material> appliedMaterials = defaultMaterials();

        // Inputs from the game thread, under inputMutex.
        mutable DX_LOCKABLE(inputMutex, "Acoustics inputs");
        dx::profile::ConditionVariable wake;
        dx::profile::ConditionVariable settled;
        // Paused while the raytracing mode is off: the worker sleeps, keeping its scene.
        bool paused = false;
        // Each setPaused starts a new epoch. A pass belongs to the epoch it started in, and only
        // commits results and counts itself while that epoch lasts, so once setPaused returns no
        // pass from before it changes anything.
        uint64_t epoch = 0;

        // What every pass simulates with, as last set.
        struct Inputs {
            Vector listenerLocation;
            Rotator listenerRotation;
            std::vector<SourceInput> sources;
            ReflectionSettings reflection;
            BakedUse bakedUse;
            int32_t tracedSource = 0;
        };

        Inputs latest;

        // What has been handed over since the last pass, which takes it all.
        struct Changes {
            std::optional<std::shared_ptr<const Geometry>> geometry;
            std::optional<std::vector<Material>> materials;
            std::optional<std::vector<MoverPose>> poses;
            std::optional<std::shared_ptr<const BakedData>> baked;
        };

        Changes changes;
        uint64_t requested = 0;
        uint64_t processed = 0;
        // What the worker has taken in, for the game thread to read.
        std::shared_ptr<const Geometry> current;
        Stats published;

        // Results for the mixer, under resultMutex, from passes of resultEpoch.
        mutable DX_LOCKABLE(resultMutex, "Acoustics results");
        uint64_t resultEpoch = 0;
        std::unordered_map<int32_t, DirectPath> directResults;
        uint64_t resultVersion = 0;
        std::array<float, 3> reverbResult {};
        uint64_t reverbVersion = 0;
        std::unordered_map<int32_t, PathResult> pathResults;
        uint64_t pathVersion = 0;
        std::vector<PathSegment> traced;

        // Runs run(), whose waits on wake end when the worker is stopped.
        workers::Worker worker;

        // The worker stops before the objects it uses go, which they then do in the reverse of
        // their declaration.
        ~Impl()
        {
            worker.stop();
        }

        void releaseScene()
        {
            // What is in the level's scene goes before it (steam_scene.h).
            movers.clear();
            staticMesh.reset();
            scene.reset();
        }

        static IPLMatrix4x4 steamMatrix(const MoverPose& pose)
        {
            std::array<std::array<float, 4>, 4> m = steamTransform(pose);
            IPLMatrix4x4 out {};
            for (size_t row = 0; row < 4; row++) {
                for (size_t column = 0; column < 4; column++) {
                    out.elements[row][column] = m[row][column];
                }
            }
            return out;
        }

        steamscene::StaticMesh makeMesh(IPLScene target, const Geometry& geometry)
        {
            return steamscene::makeMesh(api, target, geometry, appliedMaterials);
        }

        // Puts each mover where its pose says, takes out those with none (destroyed), and
        // commits the level's scene if anything moved. Only between simulations.
        void placeMovers(const std::vector<MoverPose>& poses)
        {
            appliedPoses = poses;
            if (!scene) {
                return;
            }
            bool changed = false;
            std::unordered_map<int32_t, const MoverPose*> byId;
            for (const MoverPose& pose : poses) {
                byId[pose.id] = &pose;
            }
            for (auto& [id, mover] : movers) {
                auto found = byId.find(id);
                if (found == byId.end()) {
                    if (mover.instance.added()) {
                        mover.instance.remove();
                        changed = true;
                    }
                    continue;
                }
                if (!(mover.pose == *found->second)) {
                    mover.pose = *found->second;
                    mover.instance.place(steamMatrix(mover.pose));
                    changed = true;
                }
                if (!mover.instance.added()) {
                    mover.instance.add();
                    changed = true;
                }
            }
            if (changed) {
                api->iplSceneCommit(scene.get());
            }
        }

        // Builds the scene for geometry, or an empty one, and gives it to the simulator.
        void buildScene(const Geometry* geometry, Stats& stats)
        {
            DX_ZONE_NAMED("Acoustics build scene");
            auto start = std::chrono::steady_clock::now();
            releaseScene();
            IPLSceneSettings settings {};
            settings.type = sceneType;
            settings.embreeDevice = embree.get();
            scene = steamscene::makeScene(api, settings);
            if (!scene) {
                log::error("DevAudio", "Steam Audio: the scene could not be created");
                stats.scene = false;
                return;
            }
            stats.triangles = 0;
            stats.movers = 0;
            if (geometry && !geometry->triangles.empty()) {
                staticMesh = makeMesh(scene.get(), *geometry);
                if (staticMesh) {
                    stats.triangles = geometry->triangles.size();
                    stats.materials = appliedMaterials.size();
                }
            }
            if (geometry) {
                for (const MoverShape& shape : geometry->movers) {
                    MoverInstance mover;
                    mover.scene = steamscene::makeScene(api, settings);
                    if (!mover.scene) {
                        continue;
                    }
                    mover.mesh = makeMesh(mover.scene.get(), *shape.shape);
                    api->iplSceneCommit(mover.scene.get());
                    mover.instance = steamscene::InstancedMesh::make(api, scene.get(), mover.scene.get(), steamMatrix({}));
                    if (!mover.instance) {
                        continue;
                    }
                    mover.pose.id = -1;
                    stats.triangles += shape.shape->triangles.size();
                    stats.movers++;
                    movers[shape.id] = std::move(mover);
                }
            }
            // Movers join the scene as their poses place them.
            placeMovers(appliedPoses);
            api->iplSceneCommit(scene.get());
            api->iplSimulatorSetScene(simulator.get(), scene.get());
            api->iplSimulatorCommit(simulator.get());
            // The listener's reverb source is made again for each scene: one added before the
            // simulator had this scene gives reflections that ignore it (every room then decays
            // in about 3 s, whatever its size or materials). Not in Steam Audio's documentation;
            // found by test (acoustics_test).
            listenerSource.reset();
            IPLSourceSettings sourceSettings {};
            sourceSettings.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
            listenerSource = steam::makeSource(api, simulator.get(), sourceSettings);
            api->iplSimulatorCommit(simulator.get());
            lastReflections = {};
            stats.scene = staticMesh != nullptr || !movers.empty();
            stats.buildMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }

        // Changes the materials whose values differ from what the scene has.
        void updateMaterials(const std::vector<Material>& table)
        {
            bool changed = false;
            for (size_t i = 0; i < table.size() && i < appliedMaterials.size(); i++) {
                const Material& now = table[i];
                const Material& before = appliedMaterials[i];
                if (now.absorption == before.absorption && now.scattering == before.scattering && now.transmission == before.transmission) {
                    continue;
                }
                appliedMaterials[i] = now;
                IPLMaterial material = steamMaterial(now);
                if (staticMesh) {
                    api->iplStaticMeshSetMaterial(staticMesh.get(), scene.get(), &material, static_cast<IPLint32>(i));
                    changed = true;
                }
                for (auto& [id, mover] : movers) {
                    if (mover.mesh) {
                        api->iplStaticMeshSetMaterial(mover.mesh.get(), mover.scene.get(), &material, static_cast<IPLint32>(i));
                    }
                }
            }
            if (changed) {
                for (auto& [id, mover] : movers) {
                    api->iplSceneCommit(mover.scene.get());
                }
                api->iplSceneCommit(scene.get());
            }
        }

        // Adds and removes simulator sources to match the voices asked for.
        void syncSources(const std::vector<SourceInput>& inputs)
        {
            bool changed = false;
            std::unordered_map<int32_t, bool> wanted;
            for (const SourceInput& input : inputs) {
                wanted[input.id] = true;
                if (sources.count(input.id)) {
                    continue;
                }
                IPLSourceSettings settings {};
                settings.flags = canBake ? static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_PATHING) : IPL_SIMULATIONFLAGS_DIRECT;
                steam::Source source = steam::makeSource(api, simulator.get(), settings);
                if (!source) {
                    continue;
                }
                sources[input.id] = std::move(source);
                changed = true;
            }
            for (auto it = sources.begin(); it != sources.end();) {
                if (wanted.count(it->first)) {
                    ++it;
                    continue;
                }
                it = sources.erase(it);
                changed = true;
            }
            if (changed) {
                api->iplSimulatorCommit(simulator.get());
            }
        }

        // Occlusion and transmission for every source, from the listener.
        std::unordered_map<int32_t, DirectPath> simulateDirect(const std::vector<SourceInput>& inputs, const Vector& listener, const Rotator& rotation, Stats& stats)
        {
            DX_ZONE_NAMED("Acoustics simulate direct");
            std::unordered_map<int32_t, DirectPath> paths;
            stats.sources = 0;
            if (!scene || (!staticMesh && movers.empty()) || inputs.empty()) {
                return paths;
            }
            IPLSimulationSharedInputs shared {};
            shared.listener = steamSpace(listener, rotation);
            api->iplSimulatorSetSharedInputs(simulator.get(), IPL_SIMULATIONFLAGS_DIRECT, &shared);
            for (const SourceInput& input : inputs) {
                auto found = sources.find(input.id);
                if (found == sources.end()) {
                    continue;
                }
                IPLSimulationInputs source {};
                source.flags = IPL_SIMULATIONFLAGS_DIRECT;
                source.directFlags = static_cast<IPLDirectSimulationFlags>(IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
                source.source = steamSpace(input.location, {});
                source.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
                source.occlusionRadius = OcclusionRadius;
                source.numOcclusionSamples = OcclusionSamples;
                source.numTransmissionRays = TransmissionSurfaces;
                api->iplSourceSetInputs(found->second.get(), IPL_SIMULATIONFLAGS_DIRECT, &source);
            }
            auto start = std::chrono::steady_clock::now();
            api->iplSimulatorRunDirect(simulator.get());
            stats.directMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            for (const SourceInput& input : inputs) {
                auto found = sources.find(input.id);
                if (found == sources.end()) {
                    continue;
                }
                IPLSimulationOutputs outputs {};
                api->iplSourceGetOutputs(found->second.get(), IPL_SIMULATIONFLAGS_DIRECT, &outputs);
                DirectPath path;
                path.occlusion = outputs.direct.occlusion;
                for (size_t band = 0; band < 3; band++) {
                    path.transmission[band] = outputs.direct.transmission[band];
                }
                paths[input.id] = path;
            }
            stats.sources = paths.size();
            return paths;
        }

        // The reverb around the listener: rays from a source at the listener, reflected around
        // the scene. Returns whether it ran.
        bool simulateReflections(const Vector& listener, const Rotator& rotation, const ReflectionSettings& settings, Stats& stats)
        {
            DX_ZONE_NAMED("Acoustics simulate reflections");
            if (!scene || (!staticMesh && movers.empty())) {
                stats.reverbTimes = {};
                return true;
            }
            auto now = std::chrono::steady_clock::now();
            if (now - lastReflections < ReflectionInterval) {
                return false;
            }
            lastReflections = now;
            if (!listenerSource) {
                return false;
            }
            IPLSimulationSharedInputs shared {};
            shared.listener = steamSpace(listener, rotation);
            shared.numRays = std::clamp(settings.rays, 1, MaxRays);
            shared.numBounces = std::max(settings.bounces, 1);
            shared.duration = ReverbDuration;
            shared.order = 0;
            shared.irradianceMinDistance = 1.0f;
            api->iplSimulatorSetSharedInputs(simulator.get(), IPL_SIMULATIONFLAGS_REFLECTIONS, &shared);
            auto simulate = [&](bool baked) {
                IPLSimulationInputs inputs {};
                inputs.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
                inputs.source = shared.listener;
                inputs.reverbScale[0] = inputs.reverbScale[1] = inputs.reverbScale[2] = 1.0f;
                if (baked) {
                    inputs.baked = IPL_TRUE;
                    inputs.bakedDataIdentifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;
                    inputs.bakedDataIdentifier.variation = IPL_BAKEDDATAVARIATION_REVERB;
                }
                api->iplSourceSetInputs(listenerSource.get(), IPL_SIMULATIONFLAGS_REFLECTIONS, &inputs);
                api->iplSimulatorRunReflections(simulator.get());
                IPLSimulationOutputs outputs {};
                api->iplSourceGetOutputs(listenerSource.get(), IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);
                for (size_t band = 0; band < 3; band++) {
                    stats.reverbTimes[band] = outputs.reflections.reverbTimes[band];
                }
            };
            // The baked reverb where a probe is near the listener; the live one elsewhere (a
            // listener off every floor's probes gets nothing baked), or when it is asked for.
            stats.reverbBaked = false;
            if (batch && settings.baked) {
                simulate(true);
                stats.reverbBaked = stats.reverbTimes[0] > 0 || stats.reverbTimes[1] > 0 || stats.reverbTimes[2] > 0;
            }
            if (!stats.reverbBaked) {
                simulate(false);
            }
            stats.rays = shared.numRays;
            stats.bounces = shared.numBounces;
            stats.reflectionMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count();
            return true;
        }

        void releaseBatch()
        {
            if (batch) {
                batch.reset();
                api->iplSimulatorCommit(simulator.get());
                batchProbes = 0;
            }
        }

        // Swaps in baked data, or none. Only between simulations.
        void loadBaked(const std::shared_ptr<const BakedData>& data)
        {
            DX_ZONE_NAMED("Acoustics load baked data");
            releaseBatch();
            bakeError.clear();
            if (!data || !canBake) {
                return;
            }
            IPLSerializedObjectSettings settings {};
            settings.data = const_cast<IPLbyte*>(data->batch.data());
            settings.size = data->batch.size();
            IPLSerializedObject read = nullptr;
            if (api->iplSerializedObjectCreate(api->context, &settings, &read) != IPL_STATUS_SUCCESS) {
                bakeError = "the baked data could not be read";
                return;
            }
            steam::SerializedObject serialized(read, { api });
            IPLProbeBatch made = nullptr;
            if (api->iplProbeBatchLoad(api->context, serialized.get(), &made) != IPL_STATUS_SUCCESS || !made) {
                bakeError = "Steam Audio refused the baked data";
                return;
            }
            steam::ProbeBatch loaded(made, { api });
            api->iplProbeBatchCommit(loaded.get());
            size_t count = static_cast<size_t>(std::max(api->iplProbeBatchGetNumProbes(loaded.get()), 0));
            if (count != data->probes.size()) {
                bakeError = "the baked data holds the wrong number of probes";
                return;
            }
            batch = steam::addBatch(api, simulator.get(), std::move(loaded));
            batchProbes = count;
            api->iplSimulatorCommit(simulator.get());
        }

        static void IPLCALL tracePath(IPLVector3 from, IPLVector3 to, IPLbool occluded, void* user)
        {
            auto* legs = static_cast<std::vector<PathSegment>*>(user);
            legs->push_back({ steamscene::unreal(from), steamscene::unreal(to), occluded == IPL_TRUE });
        }

        // Steam Audio's pathing outputs as the mixer uses them. Measured against 4.8.1
        // (acoustic_bake_test): the field is first-order Ambisonics in ACN order, orthonormal,
        // with Ambisonics' forward, left and up on Steam Audio's -z, -x and +y; a path of length
        // r metres (at least 1) from unit direction n gives W = Y00 / r and the first order
        // Y1 n / r. A source in plain view gives its straight line, with an EQ of 1.
        static PathResult interpretPath(const IPLPathEffectParams& out)
        {
            constexpr float Y00 = 0.28209479f;
            constexpr float Y1 = 0.48860251f;
            PathResult path;
            for (size_t band = 0; band < 3; band++) {
                path.eq[band] = out.eqCoeffs[band];
            }
            if (out.shCoeffs) {
                for (size_t i = 0; i < 4; i++) {
                    path.field[i] = out.shCoeffs[i];
                }
            }
            float level = path.field[0] / Y00;
            if (level <= 1e-6f) {
                return path;
            }
            IPLVector3 steamWay { -path.field[1] / Y1, path.field[2] / Y1, -path.field[3] / Y1 };
            float strength = std::sqrt(steamWay.x * steamWay.x + steamWay.y * steamWay.y + steamWay.z * steamWay.z);
            path.found = true;
            for (size_t band = 0; band < 3; band++) {
                path.amplitude[band] = std::clamp(path.eq[band], 0.0f, 1.0f);
            }
            path.length = UnitsPerMetre / level;
            if (strength > 1e-6f) {
                path.direction = Vector { steamWay.x / strength, steamWay.z / strength, steamWay.y / strength };
            }
            path.focus = std::clamp(strength / level, 0.0f, 1.0f);
            return path;
        }

        // Paths round what is in the way for every source the direct simulation found at least
        // partly hidden (one in plain view gains nothing from its path, which is the straight
        // line), checked against the scene as it is now (movers where they stand).
        std::unordered_map<int32_t, PathResult> simulatePathing(const std::vector<SourceInput>& inputs, const std::unordered_map<int32_t, DirectPath>& direct, const Vector& listener, const Rotator& rotation, int32_t tracedSource, std::vector<PathSegment>* legs, Stats& stats)
        {
            DX_ZONE_NAMED("Acoustics simulate pathing");
            std::vector<const SourceInput*> candidates;
            for (const SourceInput& input : inputs) {
                auto found = direct.find(input.id);
                if (found != direct.end() && found->second.occlusion < 0.999f) {
                    candidates.push_back(&input);
                }
            }
            std::sort(candidates.begin(), candidates.end(), [](const SourceInput* a, const SourceInput* b) {
                return a->priority > b->priority;
            });
            if (candidates.size() > MaxPathedSources) {
                candidates.resize(MaxPathedSources);
            }
            std::unordered_map<int32_t, bool> chosen;
            for (const SourceInput* input : candidates) {
                chosen[input->id] = true;
            }
            auto hidden = [&](int32_t id) {
                return chosen.count(id) > 0;
            };
            std::unordered_map<int32_t, PathResult> results;
            stats.pathed = 0;
            if (!batch || !scene || inputs.empty()) {
                return results;
            }
            IPLSimulationSharedInputs shared {};
            shared.listener = steamSpace(listener, rotation);
            auto setInputs = [&](bool validateOnly, int32_t only) {
                for (const SourceInput& input : inputs) {
                    auto found = sources.find(input.id);
                    if (found == sources.end()) {
                        continue;
                    }
                    IPLSimulationInputs source {};
                    bool wanted = validateOnly ? input.id == only : hidden(input.id);
                    source.flags = wanted ? IPL_SIMULATIONFLAGS_PATHING : IPL_SIMULATIONFLAGS_DIRECT;
                    source.source = steamSpace(input.location, {});
                    source.pathingProbes = batch.get();
                    source.visRadius = PathVisRadius;
                    source.visThreshold = PathVisThreshold;
                    source.visRange = PathVisRange;
                    source.pathingOrder = 1;
                    source.enableValidation = IPL_TRUE;
                    // A path a closed door breaks is only dropped when another is searched for in
                    // its place (measured against 4.8.1: validation alone keeps it), so the search
                    // is on. It is the costly part, about 1.3 ms a source it fails for, hence the
                    // limits in simulatePathing's caller.
                    source.findAlternatePaths = IPL_TRUE;
                    api->iplSourceSetInputs(found->second.get(), IPL_SIMULATIONFLAGS_PATHING, &source);
                }
            };
            api->iplSimulatorSetSharedInputs(simulator.get(), IPL_SIMULATIONFLAGS_PATHING, &shared);
            setInputs(false, 0);
            auto start = std::chrono::steady_clock::now();
            api->iplSimulatorRunPathing(simulator.get());
            stats.pathMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            for (const SourceInput& input : inputs) {
                auto found = sources.find(input.id);
                if (found == sources.end() || !hidden(input.id)) {
                    continue;
                }
                IPLSimulationOutputs outputs {};
                api->iplSourceGetOutputs(found->second.get(), IPL_SIMULATIONFLAGS_PATHING, &outputs);
                PathResult path = interpretPath(outputs.pathing);
                stats.pathed += path.found ? 1u : 0u;
                results[input.id] = path;
            }
            // The traced source's legs, from a second run of it alone: Steam Audio reports the legs
            // it checks, for every source, without saying whose.
            if (legs && tracedSource != 0 && sources.count(tracedSource)) {
                shared.pathingVisCallback = tracePath;
                shared.pathingUserData = legs;
                api->iplSimulatorSetSharedInputs(simulator.get(), IPL_SIMULATIONFLAGS_PATHING, &shared);
                setInputs(true, tracedSource);
                api->iplSimulatorRunPathing(simulator.get());
                shared.pathingVisCallback = nullptr;
                shared.pathingUserData = nullptr;
                api->iplSimulatorSetSharedInputs(simulator.get(), IPL_SIMULATIONFLAGS_PATHING, &shared);
            }
            return results;
        }

        void run(const std::stop_token& stop)
        {
            Stats stats;
            stats.embree = sceneType == IPL_SCENETYPE_EMBREE;
            auto secondStart = std::chrono::steady_clock::now();
            size_t passes = 0;
            std::unique_lock<dx::profile::Mutex> lock(inputMutex);
            while (!stop.stop_requested()) {
                if (paused) {
                    settled.notify_all();
                    wake.wait(lock, stop, [this] {
                        return !paused;
                    });
                    continue;
                }
                wake.wait_for(lock, stop, PassInterval, [this] {
                    return requested != processed;
                });
                if (stop.stop_requested()) {
                    break;
                }
                // Paused while waiting: back to the top to sleep, without one more pass.
                if (paused) {
                    continue;
                }
                uint64_t generation = requested;
                uint64_t passEpoch = epoch;
                Inputs in = latest;
                in.reflection.baked = in.bakedUse.reverb;
                Changes taken = std::exchange(changes, {});
                bool force = generation != processed;
                lock.unlock();
                DX_ZONE_NAMED("Acoustics pass");
                if (taken.materials) {
                    updateMaterials(*taken.materials);
                }
                if (taken.poses) {
                    appliedPoses = *taken.poses;
                }
                if (taken.geometry) {
                    // Baked data belongs to the level it was baked for.
                    releaseBatch();
                    buildScene(taken.geometry->get(), stats);
                } else if (taken.poses) {
                    placeMovers(*taken.poses);
                }
                if (taken.baked) {
                    loadBaked(*taken.baked);
                }
                stats.baked = batch != nullptr;
                stats.probes = batchProbes;
                stats.bakeError = bakeError;
                syncSources(in.sources);
                std::unordered_map<int32_t, DirectPath> paths = simulateDirect(in.sources, in.listenerLocation, in.listenerRotation, stats);
                std::vector<PathSegment> legs;
                std::unordered_map<int32_t, PathResult> routes;
                bool pathingRan = false;
                if (in.bakedUse.pathing && batch) {
                    auto now = std::chrono::steady_clock::now();
                    if (force || taken.baked || now - lastPathing >= PathingInterval) {
                        lastPathing = now;
                        routes = simulatePathing(in.sources, paths, in.listenerLocation, in.listenerRotation, in.tracedSource, &legs, stats);
                        pathingRan = true;
                    }
                } else {
                    stats.pathed = 0;
                    stats.pathMilliseconds = 0;
                }
                {
                    DX_ZONE_NAMED("Acoustics commit");
                    std::lock_guard<dx::profile::Mutex> results(resultMutex);
                    if (passEpoch == resultEpoch) {
                        directResults = std::move(paths);
                        resultVersion++;
                        if (pathingRan || !in.bakedUse.pathing || !batch) {
                            pathResults = std::move(routes);
                            pathVersion++;
                            traced = std::move(legs);
                        }
                    }
                }
                // Asked to settle, the reflections are brought up to date too.
                if (force) {
                    lastReflections = {};
                }
                if (simulateReflections(in.listenerLocation, in.listenerRotation, in.reflection, stats)) {
                    DX_ZONE_NAMED("Acoustics commit reverb");
                    std::lock_guard<dx::profile::Mutex> results(resultMutex);
                    if (passEpoch == resultEpoch) {
                        reverbResult = stats.reverbTimes;
                        reverbVersion++;
                    }
                }
                lock.lock();
                if (taken.geometry) {
                    current = *taken.geometry;
                }
                // Paused (or paused and woken) while it ran, the pass is dropped: setPaused has
                // published the pause, and the next pass after it does the work again.
                if (passEpoch == epoch) {
                    passes++;
                    stats.passes++;
                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration<double>(now - secondStart).count();
                    if (elapsed >= 1.0) {
                        stats.passesPerSecond = static_cast<double>(passes) / elapsed;
                        passes = 0;
                        secondStart = now;
                    }
                    stats.paused = false;
                    published = stats;
                }
                processed = generation;
                settled.notify_all();
            }
        }
    };

    std::unique_ptr<Simulation> Simulation::create(uint32_t sampleRate, size_t channels)
    {
        const steam::Api* api = steam::api();
        if (!api || !api->acoustics) {
            log::warn("DevAudio", "AmbrosiaRaytracedAudio: Steam Audio's simulation is not available");
            return nullptr;
        }
        auto impl = std::make_unique<Impl>();
        impl->api = api;
        impl->rate = sampleRate;
        IPLAudioSettings audio {};
        audio.samplingRate = static_cast<IPLint32>(sampleRate);
        audio.frameSize = static_cast<IPLint32>(steam::BlockFrames);
        auto filters = std::make_unique<DirectFilters::Impl>();
        filters->api = api;
        filters->audio = audio;
        for (size_t i = 0; i < channels; i++) {
            steam::DirectEffect effect = makeDirectEffect(api, audio);
            if (!effect) {
                log::error("DevAudio", "Steam Audio: a direct effect could not be created");
                return nullptr;
            }
            filters->effects.push_back(std::move(effect));
        }
        impl->filters = std::unique_ptr<DirectFilters>(new DirectFilters(std::move(filters)));
        impl->canBake = api->baking;
        auto pathFilters = std::make_unique<PathFilters::Impl>();
        pathFilters->api = api;
        pathFilters->audio = audio;
        if (impl->canBake) {
            pathFilters->binaural = steam::Binaural::create(sampleRate, channels);
            for (size_t i = 0; pathFilters->binaural && i < channels; i++) {
                steam::DirectEffect effect = makeDirectEffect(api, audio);
                if (!effect) {
                    break;
                }
                pathFilters->effects.push_back(std::move(effect));
            }
            if (!pathFilters->binaural || pathFilters->effects.size() != channels) {
                log::error("DevAudio", "Steam Audio: the pathing effects could not be made, so baked paths are not used");
                impl->canBake = false;
            }
        }
        impl->pathing = std::unique_ptr<PathFilters>(new PathFilters(std::move(pathFilters)));
        auto reverb = std::make_unique<ListenerReverb::Impl>();
        reverb->api = api;
        reverb->irSize = static_cast<IPLint32>(ReverbDuration * static_cast<float>(sampleRate));
        for (size_t ear = 0; ear < 2; ear++) {
            IPLReflectionEffectSettings settings {};
            settings.type = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
            settings.irSize = reverb->irSize;
            settings.numChannels = 1;
            IPLReflectionEffect effect = nullptr;
            if (api->iplReflectionEffectCreate(api->context, &audio, &settings, &effect) != IPL_STATUS_SUCCESS) {
                log::error("DevAudio", "Steam Audio: the reverb effect could not be created");
                return nullptr;
            }
            reverb->effects[ear] = steam::ReflectionEffect(effect, { api });
            reverb->in[ear].assign(steam::BlockFrames, 0.0f);
            reverb->out[ear].assign(steam::BlockFrames, 0.0f);
        }
        impl->reverb = std::unique_ptr<ListenerReverb>(new ListenerReverb(std::move(reverb)));
        IPLEmbreeDeviceSettings embreeSettings {};
        IPLEmbreeDevice embree = nullptr;
        if (api->iplEmbreeDeviceCreate(api->context, &embreeSettings, &embree) == IPL_STATUS_SUCCESS) {
            impl->embree = steam::EmbreeDevice(embree, { api });
            impl->sceneType = IPL_SCENETYPE_EMBREE;
        } else {
            log::warn("DevAudio", "Steam Audio: Embree is not available, so the built-in ray tracer is used");
        }
        IPLSimulationSettings settings {};
        settings.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS | (impl->canBake ? IPL_SIMULATIONFLAGS_PATHING : 0));
        settings.numVisSamples = PathVisSamples;
        settings.reflectionType = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
        settings.maxNumOcclusionSamples = 32;
        settings.maxNumRays = MaxRays;
        settings.numDiffuseSamples = 32;
        settings.maxDuration = ReverbDuration;
        // First order for the way pathed sound arrives; the reverb uses order 0 regardless.
        settings.maxOrder = 1;
        settings.maxNumSources = 1;
        // Steam Audio's own threads for the simulation, within the background cap.
        settings.numThreads = std::min(2, workers::threadCap());
        settings.samplingRate = static_cast<IPLint32>(sampleRate);
        settings.frameSize = static_cast<IPLint32>(steam::BlockFrames);
        settings.sceneType = impl->sceneType;
        IPLSimulator simulator = nullptr;
        if (api->iplSimulatorCreate(api->context, &settings, &simulator) != IPL_STATUS_SUCCESS && impl->embree) {
            // The simulator's scene type is fixed for its life, so fall back as a whole.
            impl->embree.reset();
            impl->sceneType = IPL_SCENETYPE_DEFAULT;
            settings.sceneType = impl->sceneType;
            log::warn("DevAudio", "Steam Audio: no simulator with Embree, so the built-in ray tracer is used");
            if (api->iplSimulatorCreate(api->context, &settings, &simulator) != IPL_STATUS_SUCCESS) {
                simulator = nullptr;
            }
        }
        impl->simulator = steam::Simulator(simulator, { api });
        if (!impl->simulator) {
            log::error("DevAudio", "Steam Audio: the simulator could not be created");
            return nullptr;
        }

        Impl* raw = impl.get();
        raw->worker.start("Acoustics worker", [raw](std::stop_token stop) { raw->run(stop); });
        return std::unique_ptr<Simulation>(new Simulation(std::move(impl)));
    }

    void Simulation::setGeometry(std::shared_ptr<const Geometry> geometry)
    {
        {
            std::lock_guard<dx::profile::Mutex> lock(impl->inputMutex);
            impl->changes.geometry = std::move(geometry);
            impl->requested++;
        }
        impl->wake.notify_all();
    }

    void Simulation::setMaterials(const std::vector<Material>& materials)
    {
        {
            std::lock_guard<dx::profile::Mutex> lock(impl->inputMutex);
            impl->changes.materials = materials;
            impl->requested++;
        }
        impl->wake.notify_all();
    }

    void Simulation::setListener(const Vector& location, const Rotator& rotation)
    {
        std::lock_guard<dx::profile::Mutex> lock(impl->inputMutex);
        impl->latest.listenerLocation = location;
        impl->latest.listenerRotation = rotation;
    }

    void Simulation::setSources(std::vector<SourceInput> sources)
    {
        std::lock_guard<dx::profile::Mutex> lock(impl->inputMutex);
        impl->latest.sources = std::move(sources);
    }

    void Simulation::setMoverPoses(std::vector<MoverPose> poses)
    {
        std::lock_guard<dx::profile::Mutex> lock(impl->inputMutex);
        impl->changes.poses = std::move(poses);
    }

    bool Simulation::directPaths(std::unordered_map<int32_t, DirectPath>& paths, uint64_t& version) const
    {
        std::unique_lock<dx::profile::Mutex> lock(impl->resultMutex, std::try_to_lock);
        if (!lock.owns_lock() || impl->resultVersion == version) {
            return false;
        }
        paths = impl->directResults;
        version = impl->resultVersion;
        return true;
    }

    bool Simulation::reverbTimes(std::array<float, 3>& times) const
    {
        std::unique_lock<dx::profile::Mutex> lock(impl->resultMutex, std::try_to_lock);
        if (!lock.owns_lock() || impl->reverbVersion == 0) {
            return false;
        }
        times = impl->reverbResult;
        return true;
    }

    void Simulation::setReflectionSettings(const ReflectionSettings& settings)
    {
        std::lock_guard<dx::profile::Mutex> lock(impl->inputMutex);
        impl->latest.reflection = settings;
    }

    DirectFilters& Simulation::filters()
    {
        return *impl->filters;
    }

    PathFilters& Simulation::pathFilters()
    {
        return *impl->pathing;
    }

    ListenerReverb& Simulation::reverb()
    {
        return *impl->reverb;
    }

    void Simulation::setBaked(std::shared_ptr<const BakedData> data)
    {
        {
            std::lock_guard<dx::profile::Mutex> lock(impl->inputMutex);
            impl->changes.baked = std::move(data);
            impl->requested++;
        }
        impl->wake.notify_all();
    }

    void Simulation::setBakedUse(const BakedUse& use)
    {
        {
            std::lock_guard<dx::profile::Mutex> lock(impl->inputMutex);
            if (impl->latest.bakedUse == use) {
                return;
            }
            impl->latest.bakedUse = use;
            impl->requested++;
        }
        impl->wake.notify_all();
    }

    bool Simulation::pathResults(std::unordered_map<int32_t, PathResult>& paths, uint64_t& version) const
    {
        std::unique_lock<dx::profile::Mutex> lock(impl->resultMutex, std::try_to_lock);
        if (!lock.owns_lock() || impl->pathVersion == version) {
            return false;
        }
        paths = impl->pathResults;
        version = impl->pathVersion;
        return true;
    }

    void Simulation::setTracedSource(int32_t id)
    {
        std::lock_guard<dx::profile::Mutex> lock(impl->inputMutex);
        impl->latest.tracedSource = id;
    }

    std::vector<PathSegment> Simulation::tracedPath() const
    {
        std::lock_guard<dx::profile::Mutex> lock(impl->resultMutex);
        return impl->traced;
    }

    bool Simulation::canUseBaked() const
    {
        return impl->canBake;
    }

    std::shared_ptr<const Geometry> Simulation::geometry() const
    {
        std::lock_guard<dx::profile::Mutex> lock(impl->inputMutex);
        return impl->current;
    }

    Stats Simulation::stats() const
    {
        std::lock_guard<dx::profile::Mutex> lock(impl->inputMutex);
        return impl->published;
    }

    void Simulation::setPaused(bool paused)
    {
        uint64_t epoch = 0;
        {
            std::lock_guard<dx::profile::Mutex> lock(impl->inputMutex);
            impl->paused = paused;
            impl->published.paused = paused;
            epoch = ++impl->epoch;
            // Coming back, a pass straight away; going to sleep, none.
            if (!paused) {
                impl->requested++;
            }
        }
        {
            std::lock_guard<dx::profile::Mutex> results(impl->resultMutex);
            impl->resultEpoch = epoch;
            if (paused) {
                // What was simulated before is stale by the time the mode comes back.
                impl->directResults.clear();
                impl->resultVersion++;
                impl->pathResults.clear();
                impl->pathVersion++;
                impl->traced.clear();
                impl->reverbResult = {};
            }
        }
        impl->wake.notify_all();
    }

    void Simulation::settle()
    {
        std::unique_lock<dx::profile::Mutex> lock(impl->inputMutex);
        if (impl->paused) {
            return;
        }
        uint64_t target = ++impl->requested;
        impl->wake.notify_all();
        // A pause that comes first means the pass waited for never runs; the worker's parking
        // wakes this.
        impl->settled.wait(lock, [&] {
            return impl->processed >= target || impl->paused;
        });
    }
#else
    struct DirectFilters::Impl {};

    void DirectFilters::apply(size_t, float*, const DirectPath&)
    {
    }

    void DirectFilters::reset(size_t)
    {
    }

    void DirectFilters::renew()
    {
    }

    struct ListenerReverb::Impl {};

    void ListenerReverb::process(const float*, float*, const std::array<float, 3>&, float)
    {
    }

    void ListenerReverb::reset()
    {
    }

    struct PathFilters::Impl {};

    void PathFilters::apply(size_t, float*, const std::array<float, 3>&, float*, float*, steam::Direction, float)
    {
    }

    bool PathFilters::tail(size_t, float*, float*)
    {
        return false;
    }

    void PathFilters::reset(size_t)
    {
    }

    void PathFilters::renew()
    {
    }

    struct Simulation::Impl {
        std::unique_ptr<DirectFilters> filters;
        std::unique_ptr<PathFilters> pathing;
        std::unique_ptr<ListenerReverb> reverb;
    };

    void Simulation::setBaked(std::shared_ptr<const BakedData>)
    {
    }

    void Simulation::setBakedUse(const BakedUse&)
    {
    }

    bool Simulation::pathResults(std::unordered_map<int32_t, PathResult>&, uint64_t&) const
    {
        return false;
    }

    void Simulation::setTracedSource(int32_t)
    {
    }

    std::vector<PathSegment> Simulation::tracedPath() const
    {
        return {};
    }

    bool Simulation::canUseBaked() const
    {
        return false;
    }

    PathFilters& Simulation::pathFilters()
    {
        return *impl->pathing;
    }

    std::unique_ptr<Simulation> Simulation::create(uint32_t, size_t)
    {
        log::warn("DevAudio", "AmbrosiaRaytracedAudio: this build has no Steam Audio (AMBROSIA_STEAM_AUDIO is off)");
        return nullptr;
    }

    void Simulation::setGeometry(std::shared_ptr<const Geometry>)
    {
    }

    void Simulation::setMaterials(const std::vector<Material>&)
    {
    }

    void Simulation::setListener(const Vector&, const Rotator&)
    {
    }

    void Simulation::setSources(std::vector<SourceInput>)
    {
    }

    void Simulation::setMoverPoses(std::vector<MoverPose>)
    {
    }

    bool Simulation::directPaths(std::unordered_map<int32_t, DirectPath>&, uint64_t&) const
    {
        return false;
    }

    bool Simulation::reverbTimes(std::array<float, 3>&) const
    {
        return false;
    }

    void Simulation::setReflectionSettings(const ReflectionSettings&)
    {
    }

    DirectFilters& Simulation::filters()
    {
        return *impl->filters;
    }

    ListenerReverb& Simulation::reverb()
    {
        return *impl->reverb;
    }

    std::shared_ptr<const Geometry> Simulation::geometry() const
    {
        return nullptr;
    }

    Stats Simulation::stats() const
    {
        return {};
    }

    void Simulation::setPaused(bool)
    {
    }

    void Simulation::settle()
    {
    }
#endif

    DirectFilters::DirectFilters(std::unique_ptr<Impl> implementation)
        : impl(std::move(implementation))
    {
    }

    DirectFilters::~DirectFilters() = default;

    PathFilters::PathFilters(std::unique_ptr<Impl> implementation)
        : impl(std::move(implementation))
    {
    }

    PathFilters::~PathFilters() = default;

    ListenerReverb::ListenerReverb(std::unique_ptr<Impl> implementation)
        : impl(std::move(implementation))
    {
    }

    ListenerReverb::~ListenerReverb() = default;

    Simulation::Simulation(std::unique_ptr<Impl> implementation)
        : impl(std::move(implementation))
    {
    }

    Simulation::~Simulation() = default;
}

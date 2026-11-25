#include "audio/acoustic_bake.h"

#include "core/profile.h"
#include "core/workers.h"

#if DX_STEAM_AUDIO
#include "audio/steam_api.h"
#include "audio/steam_handles.h"
#include "audio/steam_scene.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>

namespace dx::audio::acoustics {
    const char* bakeStageName(BakeStage stage)
    {
        switch (stage) {
        case BakeStage::Probes:
            return "placing probes";
        case BakeStage::Reverb:
            return "baking reverb";
        case BakeStage::Pathing:
            return "baking paths";
        case BakeStage::Saving:
            return "saving";
        }
        return "";
    }

    namespace {
        std::vector<Probe> placeProbesAt(const Geometry& geometry, float spacingMetres, float heightMetres)
        {
            std::vector<Probe> probes;
            if (geometry.vertices.empty() || spacingMetres <= 0) {
                return probes;
            }
            Vector low { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
            Vector high { -low.x, -low.y, -low.z };
            for (const std::array<int32_t, 3>& triangle : geometry.triangles) {
                for (int32_t index : triangle) {
                    const Vector& v = geometry.vertices[static_cast<size_t>(index)];
                    low = { std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z) };
                    high = { std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z) };
                }
            }
            float spacing = spacingMetres * UnitsPerMetre;
            float height = heightMetres * UnitsPerMetre;
            if (geometry.space.nodes.empty()) {
                // No BSP (a scene made by hand): floors are the triangles facing up, with nothing
                // between them and the probe above.
                struct Flat {
                    Vector a, b, c;
                    bool up = false;
                };

                std::vector<Flat> flats;
                for (const std::array<int32_t, 3>& triangle : geometry.triangles) {
                    Flat flat { geometry.vertices[static_cast<size_t>(triangle[0])], geometry.vertices[static_cast<size_t>(triangle[1])], geometry.vertices[static_cast<size_t>(triangle[2])] };
                    Vector front = (flat.b - flat.a).cross(flat.c - flat.a) * -1.0f;
                    float length = std::sqrt(front.dot(front));
                    flat.up = length > 0 && front.z / length > 0.7f;
                    flats.push_back(flat);
                }
                // The height at which the vertical line through x, y crosses a triangle, if it does.
                auto crossing = [](const Flat& f, float x, float y, float& z) {
                    float d = (f.b.y - f.c.y) * (f.a.x - f.c.x) + (f.c.x - f.b.x) * (f.a.y - f.c.y);
                    if (std::fabs(d) < 1e-6f) {
                        return false;
                    }
                    float u = ((f.b.y - f.c.y) * (x - f.c.x) + (f.c.x - f.b.x) * (y - f.c.y)) / d;
                    float v = ((f.c.y - f.a.y) * (x - f.c.x) + (f.a.x - f.c.x) * (y - f.c.y)) / d;
                    float w = 1 - u - v;
                    if (u < 0 || v < 0 || w < 0) {
                        return false;
                    }
                    z = u * f.a.z + v * f.b.z + w * f.c.z;
                    return true;
                };
                for (float x = low.x + spacing * 0.5f; x < high.x; x += spacing) {
                    for (float y = low.y + spacing * 0.5f; y < high.y; y += spacing) {
                        std::vector<float> floors;
                        std::vector<float> all;
                        for (const Flat& flat : flats) {
                            float z = 0;
                            if (crossing(flat, x, y, z)) {
                                all.push_back(z);
                                if (flat.up) {
                                    floors.push_back(z);
                                }
                            }
                        }
                        for (float floor : floors) {
                            bool room = std::none_of(all.begin(), all.end(), [&](float z) {
                                return z > floor + 1.0f && z < floor + height + 1.0f;
                            });
                            bool duplicate = std::any_of(probes.begin(), probes.end(), [&](const Probe& p) {
                                return p.location.x == x && p.location.y == y && std::fabs(p.location.z - (floor + height)) < 1.0f;
                            });
                            if (room && !duplicate) {
                                probes.push_back({ { x, y, floor + height }, spacing });
                            }
                        }
                    }
                }
                return probes;
            }
            // Columns are sampled this far apart going down, and a floor found between two samples
            // is narrowed to within a unit.
            constexpr float Step = 8.0f;
            const Space& space = geometry.space;
            for (float x = low.x + spacing * 0.5f; x < high.x; x += spacing) {
                for (float y = low.y + spacing * 0.5f; y < high.y; y += spacing) {
                    bool above = space.open({ x, y, high.z });
                    for (float z = high.z - Step; z > low.z - Step; z -= Step) {
                        bool here = space.open({ x, y, z });
                        if (above && !here) {
                            float top = z + Step;
                            float bottom = z;
                            while (top - bottom > 1.0f) {
                                float middle = (top + bottom) * 0.5f;
                                (space.open({ x, y, middle }) ? top : bottom) = middle;
                            }
                            // Room above the floor: open all the way up to the probe.
                            Vector at { x, y, top + height };
                            bool room = true;
                            for (float check = top + Step; check <= at.z; check += Step) {
                                room = room && space.open({ x, y, check });
                            }
                            if (room && space.open(at)) {
                                probes.push_back({ at, spacing });
                            }
                        }
                        above = here;
                    }
                }
            }
            return probes;
        }

    }

    std::vector<Probe> placeProbes(const Geometry& geometry, const BakeSettings& settings, float* spacingUsed)
    {
        // Pathing data and the bake's memory grow with the square of the probe count, so a
        // level too big for maxProbes at the wanted spacing gets wider spacing, in half metres,
        // until it fits.
        float spacing = settings.spacing;
        std::vector<Probe> probes = placeProbesAt(geometry, spacing, settings.height);
        for (int attempt = 0; settings.maxProbes > 0 && probes.size() > static_cast<size_t>(settings.maxProbes) && attempt < 16; attempt++) {
            float scale = std::sqrt(static_cast<float>(probes.size()) / static_cast<float>(settings.maxProbes));
            float wider = std::ceil(spacing * std::max(scale, 1.05f) * 2.0f) / 2.0f;
            spacing = std::max(wider, spacing + 0.5f);
            probes = placeProbesAt(geometry, spacing, settings.height);
        }
        if (spacingUsed) {
            *spacingUsed = spacing;
        }
        return probes;
    }

#if DX_STEAM_AUDIO
    namespace {
        // Steam Audio runs one bake at a time.
        dx::profile::Mutex& bakeMutex()
        {
            static DX_LOCKABLE(mutex, "Steam Audio bake");
            return mutex;
        }

        struct ProgressContext {
            const steam::Api* api = nullptr;
            const BakeProgress* progress = nullptr;
            BakeStage stage = BakeStage::Probes;
            std::stop_token stop;

            bool stopped() const
            {
                return stop.stop_requested();
            }
        };

        void IPLCALL reportProgress(IPLfloat32 fraction, void* user)
        {
            auto* context = static_cast<ProgressContext*>(user);
            // Steam Audio stops its reverb bake at the next chance once told. Its path bake is
            // left to finish, its result then thrown away: cancelling it part way fails an
            // assertion in 4.8.1's ray tracer (ray.valid() in bvh_intersector1.cpp) and ends the
            // program, while it is the short part of a bake (under 2 s for the largest levels).
            if (context->stopped()) {
                if (context->stage == BakeStage::Reverb) {
                    context->api->iplReflectionsBakerCancelBake(context->api->context);
                }
                return;
            }
            if (context->progress && *context->progress) {
                (*context->progress)(context->stage, fraction);
            }
        }

        double millisecondsSince(std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }

        // What a bake makes, released whichever way it ends: in the reverse of this order, so the
        // mesh before its scene (steam_scene.h) and both before the device the scene was made with.
        struct Objects {
            steam::EmbreeDevice embree;
            steamscene::Scene scene;
            steamscene::StaticMesh mesh;
            steam::ProbeBatch batch;
            steam::SerializedObject serialized;
        };

        std::unique_ptr<BakedData> fail(std::string* error, const char* why)
        {
            if (error) {
                *error = why;
            }
            return nullptr;
        }
    }

    bool bakingAvailable()
    {
        const steam::Api* api = steam::api();
        return api && api->baking;
    }

    std::unique_ptr<BakedData> bake(const Geometry& geometry, const std::vector<Material>& materials, const BakeSettings& settings, const BakeProgress& progress, std::string* error, std::stop_token stop)
    {
        const steam::Api* api = steam::api();
        if (!api || !api->baking) {
            return fail(error, "Steam Audio's baking is not available");
        }
        DX_ZONE_NAMED("Bake acoustics");
        std::lock_guard<dx::profile::Mutex> lock(bakeMutex());
        if (geometry.triangles.empty()) {
            return fail(error, "the level has no geometry to bake");
        }
        auto data = std::make_unique<BakedData>();
        Objects objects;
        ProgressContext context { api, &progress, BakeStage::Probes, stop };
        auto step = [&](BakeStage stage, float fraction) {
            context.stage = stage;
            reportProgress(fraction, &context);
        };
        int32_t threads = settings.threads > 0 ? std::min(settings.threads, workers::threadCap()) : workers::threadCap();

        step(BakeStage::Probes, 0);
        auto start = std::chrono::steady_clock::now();
        IPLSceneType sceneType = IPL_SCENETYPE_DEFAULT;
        IPLEmbreeDeviceSettings embreeSettings {};
        IPLEmbreeDevice embree = nullptr;
        if (api->iplEmbreeDeviceCreate(api->context, &embreeSettings, &embree) == IPL_STATUS_SUCCESS) {
            objects.embree = steam::EmbreeDevice(embree, { api });
            sceneType = IPL_SCENETYPE_EMBREE;
        }
        IPLSceneSettings sceneSettings {};
        sceneSettings.type = sceneType;
        sceneSettings.embreeDevice = objects.embree.get();
        objects.scene = steamscene::makeScene(api, sceneSettings);
        if (!objects.scene) {
            return fail(error, "the scene could not be created");
        }
        // The static level only: movers are left out, so every door is baked open.
        objects.mesh = steamscene::makeMesh(api, objects.scene.get(), geometry, materials);
        if (!objects.mesh) {
            return fail(error, "the level's mesh could not be created");
        }
        api->iplSceneCommit(objects.scene.get());

        // Probes over every floor of the open space (placeProbes says why not Steam Audio's).
        IPLProbeBatch batch = nullptr;
        if (api->iplProbeBatchCreate(api->context, &batch) != IPL_STATUS_SUCCESS) {
            return fail(error, "the probe batch could not be created");
        }
        objects.batch = steam::ProbeBatch(batch, { api });
        data->probes = placeProbes(geometry, settings, &data->spacing);
        if (settings.maxProbes > 0 && data->probes.size() > static_cast<size_t>(settings.maxProbes)) {
            return fail(error, "the level needs more probes than the cap allows at any spacing tried");
        }
        data->candidates = data->probes.size();
        for (const Probe& probe : data->probes) {
            IPLSphere sphere {};
            sphere.center = steamscene::point(probe.location);
            sphere.radius = probe.radius / UnitsPerMetre;
            api->iplProbeBatchAddProbe(objects.batch.get(), sphere);
        }
        api->iplProbeBatchCommit(objects.batch.get());
        data->probeMilliseconds = millisecondsSince(start);
        if (data->probes.empty()) {
            return fail(error, "no probes fit in the level's open space");
        }
        if (context.stopped()) {
            return fail(error, "cancelled");
        }

        step(BakeStage::Reverb, 0);
        start = std::chrono::steady_clock::now();
        IPLReflectionsBakeParams reverb {};
        reverb.scene = objects.scene.get();
        reverb.probeBatch = objects.batch.get();
        reverb.sceneType = sceneType;
        reverb.identifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;
        reverb.identifier.variation = IPL_BAKEDDATAVARIATION_REVERB;
        reverb.bakeFlags = IPL_REFLECTIONSBAKEFLAGS_BAKEPARAMETRIC;
        reverb.numRays = settings.reverbRays;
        reverb.numDiffuseSamples = 32;
        reverb.numBounces = settings.reverbBounces;
        reverb.simulatedDuration = settings.reverbDuration;
        reverb.savedDuration = settings.reverbDuration;
        reverb.order = 0;
        reverb.numThreads = threads;
        reverb.irradianceMinDistance = 1.0f;
        reverb.bakeBatchSize = 1;
        api->iplReflectionsBakerBake(api->context, &reverb, reportProgress, &context);
        data->reverbMilliseconds = millisecondsSince(start);
        data->reverbBytes = api->iplProbeBatchGetDataSize(objects.batch.get(), &reverb.identifier);
        if (context.stopped()) {
            return fail(error, "cancelled");
        }

        step(BakeStage::Pathing, 0);
        start = std::chrono::steady_clock::now();
        IPLPathBakeParams paths {};
        paths.scene = objects.scene.get();
        paths.probeBatch = objects.batch.get();
        paths.identifier.type = IPL_BAKEDDATATYPE_PATHING;
        paths.identifier.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
        paths.numSamples = settings.pathSamples;
        paths.radius = settings.pathRadius;
        paths.threshold = settings.pathThreshold;
        paths.visRange = settings.visRange;
        paths.pathRange = settings.pathRange;
        paths.numThreads = threads;
        api->iplPathBakerBake(api->context, &paths, reportProgress, &context);
        data->pathMilliseconds = millisecondsSince(start);
        data->pathBytes = api->iplProbeBatchGetDataSize(objects.batch.get(), &paths.identifier);
        if (context.stopped()) {
            return fail(error, "cancelled");
        }

        step(BakeStage::Saving, 0);
        IPLSerializedObjectSettings serializedSettings {};
        IPLSerializedObject serialized = nullptr;
        if (api->iplSerializedObjectCreate(api->context, &serializedSettings, &serialized) != IPL_STATUS_SUCCESS) {
            return fail(error, "the baked data could not be saved");
        }
        objects.serialized = steam::SerializedObject(serialized, { api });
        api->iplProbeBatchSave(objects.batch.get(), serialized);
        IPLsize size = api->iplSerializedObjectGetSize(serialized);
        const IPLbyte* bytes = api->iplSerializedObjectGetData(serialized);
        if (!bytes || size == 0) {
            return fail(error, "the baked data saved empty");
        }
        data->batch.assign(bytes, bytes + size);
        step(BakeStage::Saving, 1);
        return data;
    }
#else
    bool bakingAvailable()
    {
        return false;
    }

    std::unique_ptr<BakedData> bake(const Geometry&, const std::vector<Material>&, const BakeSettings&, const BakeProgress&, std::string* error, std::stop_token)
    {
        if (error) {
            *error = "this build has no Steam Audio (AMBROSIA_STEAM_AUDIO is off)";
        }
        return nullptr;
    }
#endif
}

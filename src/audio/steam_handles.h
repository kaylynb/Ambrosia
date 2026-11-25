#pragma once

// Handles that release Steam Audio objects when they go. Only for code built with DX_STEAM_AUDIO.
// The scene and its meshes have their own, in steam_scene.h.

#if DX_STEAM_AUDIO
#include "audio/steam_api.h"

#include <memory>
#include <type_traits>

namespace dx::audio::steam {
    // Releases an object that nothing else holds a place for, with the Api's function Release.
    template <typename Handle, auto Release>
    struct Releaser {
        const Api* api = nullptr;

        void operator()(Handle handle) const
        {
            (api->*Release)(&handle);
        }
    };

    template <typename Handle, auto Release>
    using Owned = std::unique_ptr<std::remove_pointer_t<Handle>, Releaser<Handle, Release>>;

    using EmbreeDevice = Owned<IPLEmbreeDevice, &Api::iplEmbreeDeviceRelease>;
    using HRTF = Owned<IPLHRTF, &Api::iplHRTFRelease>;
    using BinauralEffect = Owned<IPLBinauralEffect, &Api::iplBinauralEffectRelease>;
    using DirectEffect = Owned<IPLDirectEffect, &Api::iplDirectEffectRelease>;
    using ReflectionEffect = Owned<IPLReflectionEffect, &Api::iplReflectionEffectRelease>;
    using ProbeBatch = Owned<IPLProbeBatch, &Api::iplProbeBatchRelease>;
    using SerializedObject = Owned<IPLSerializedObject, &Api::iplSerializedObjectRelease>;

    // Adding a source or probe batch to a simulator, or taking it out, only takes effect when the
    // simulator is committed, as with meshes and scenes (steam_scene.h). A source's or batch's
    // handle takes it out of its simulator and releases it; the simulator's handle commits, which
    // frees what was taken out, and releases the simulator. So a simulator's handle must go after
    // the handles of what is in it: declare it before them.
    struct SimulatorRelease {
        const Api* api = nullptr;

        void operator()(IPLSimulator simulator) const
        {
            api->iplSimulatorCommit(simulator);
            api->iplSimulatorRelease(&simulator);
        }
    };

    struct SourceRelease {
        const Api* api = nullptr;
        IPLSimulator simulator = nullptr;

        void operator()(IPLSource source) const
        {
            api->iplSourceRemove(source, simulator);
            api->iplSourceRelease(&source);
        }
    };

    struct SimulatorBatchRelease {
        const Api* api = nullptr;
        IPLSimulator simulator = nullptr;

        void operator()(IPLProbeBatch batch) const
        {
            api->iplSimulatorRemoveProbeBatch(simulator, batch);
            api->iplProbeBatchRelease(&batch);
        }
    };

    using Simulator = std::unique_ptr<std::remove_pointer_t<IPLSimulator>, SimulatorRelease>;
    // A source added to the simulator it was made for.
    using Source = std::unique_ptr<std::remove_pointer_t<IPLSource>, SourceRelease>;
    // A probe batch added to a simulator.
    using SimulatorBatch = std::unique_ptr<std::remove_pointer_t<IPLProbeBatch>, SimulatorBatchRelease>;

    // A source made for simulator and added to it, or none when Steam Audio refuses it. It joins
    // the simulation at the simulator's next commit.
    inline Source makeSource(const Api* api, IPLSimulator simulator, const IPLSourceSettings& settings)
    {
        IPLSourceSettings copy = settings;
        IPLSource created = nullptr;
        if (api->iplSourceCreate(simulator, &copy, &created) != IPL_STATUS_SUCCESS) {
            return Source(nullptr, { api, simulator });
        }
        api->iplSourceAdd(created, simulator);
        return Source(created, { api, simulator });
    }

    // Adds batch to simulator, which then holds it until the handle goes. It joins the
    // simulation at the simulator's next commit.
    inline SimulatorBatch addBatch(const Api* api, IPLSimulator simulator, ProbeBatch batch)
    {
        IPLProbeBatch added = batch.release();
        api->iplSimulatorAddProbeBatch(simulator, added);
        return SimulatorBatch(added, { api, simulator });
    }
}
#endif

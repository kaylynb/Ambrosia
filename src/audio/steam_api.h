#pragma once

// The Steam Audio library as loaded at run time, for the audio code built with DX_STEAM_AUDIO.
// Each function is a pointer of the same name as in phonon.h.

#if DX_STEAM_AUDIO
#include <phonon.h>

// What the HRTF mode needs; without any of these Steam Audio is not used at all.
#define DX_STEAM_CORE_FUNCTIONS(X) \
    X(iplContextCreate)            \
    X(iplContextRelease)           \
    X(iplHRTFCreate)               \
    X(iplHRTFRelease)              \
    X(iplBinauralEffectCreate)     \
    X(iplBinauralEffectRelease)    \
    X(iplBinauralEffectReset)      \
    X(iplBinauralEffectApply)      \
    X(iplBinauralEffectGetTail)

// What the raytraced mode adds; without any of these that mode is unavailable.
#define DX_STEAM_ACOUSTIC_FUNCTIONS(X)  \
    X(iplEmbreeDeviceCreate)            \
    X(iplEmbreeDeviceRelease)           \
    X(iplSceneCreate)                   \
    X(iplSceneRelease)                  \
    X(iplSceneCommit)                   \
    X(iplStaticMeshCreate)              \
    X(iplStaticMeshRelease)             \
    X(iplStaticMeshAdd)                 \
    X(iplStaticMeshRemove)              \
    X(iplStaticMeshSetMaterial)         \
    X(iplInstancedMeshCreate)           \
    X(iplInstancedMeshRelease)          \
    X(iplInstancedMeshAdd)              \
    X(iplInstancedMeshRemove)           \
    X(iplInstancedMeshUpdateTransform)  \
    X(iplSimulatorCreate)               \
    X(iplSimulatorRelease)              \
    X(iplSimulatorSetScene)             \
    X(iplSimulatorSetSharedInputs)      \
    X(iplSimulatorCommit)               \
    X(iplSimulatorRunDirect)            \
    X(iplSimulatorRunReflections)       \
    X(iplSourceCreate)                  \
    X(iplSourceRelease)                 \
    X(iplSourceAdd)                     \
    X(iplSourceRemove)                  \
    X(iplSourceSetInputs)               \
    X(iplSourceGetOutputs)              \
    X(iplDirectEffectCreate)            \
    X(iplDirectEffectRelease)           \
    X(iplDirectEffectReset)             \
    X(iplDirectEffectApply)             \
    X(iplReflectionEffectCreate)        \
    X(iplReflectionEffectRelease)       \
    X(iplReflectionEffectReset)         \
    X(iplReflectionEffectApply)         \
    X(iplAmbisonicsDecodeEffectCreate)  \
    X(iplAmbisonicsDecodeEffectRelease) \
    X(iplAmbisonicsDecodeEffectReset)   \
    X(iplAmbisonicsDecodeEffectApply)

// What baked acoustics add to the raytraced mode: probes, baking, saving and loading the baked
// data, and pathing. Without any of these the mode runs on its live simulation alone.
#define DX_STEAM_BAKE_FUNCTIONS(X)   \
    X(iplSerializedObjectCreate)     \
    X(iplSerializedObjectRelease)    \
    X(iplSerializedObjectGetSize)    \
    X(iplSerializedObjectGetData)    \
    X(iplProbeBatchCreate)           \
    X(iplProbeBatchRelease)          \
    X(iplProbeBatchLoad)             \
    X(iplProbeBatchSave)             \
    X(iplProbeBatchGetNumProbes)     \
    X(iplProbeBatchAddProbe)         \
    X(iplProbeBatchCommit)           \
    X(iplProbeBatchGetDataSize)      \
    X(iplProbeBatchGetReverb)        \
    X(iplReflectionsBakerBake)       \
    X(iplReflectionsBakerCancelBake) \
    X(iplPathBakerBake)              \
    X(iplSimulatorAddProbeBatch)     \
    X(iplSimulatorRemoveProbeBatch)  \
    X(iplSimulatorRunPathing)

namespace dx::audio::steam {
    struct Api {
#define DX_STEAM_FIELD(name) decltype(&::name) name = nullptr;
        DX_STEAM_CORE_FUNCTIONS(DX_STEAM_FIELD)
        DX_STEAM_ACOUSTIC_FUNCTIONS(DX_STEAM_FIELD)
        DX_STEAM_BAKE_FUNCTIONS(DX_STEAM_FIELD)
#undef DX_STEAM_FIELD
        IPLContext context = nullptr;
        // Whether every function of the raytraced mode was found.
        bool acoustics = false;
        // Whether every function of baked acoustics was found too.
        bool baking = false;
    };

    // The library and its context, loaded on first use and kept for the life of the program, or
    // null when it is missing or unusable.
    const Api* api();
}
#endif

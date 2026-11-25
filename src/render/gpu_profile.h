#pragma once

// GPU zones for profiling builds, timed with OpenGL timestamp queries, beside core/profile.h's
// CPU zones. Empty unless DX_PROFILE is 1. The game makes the context once its GL context is
// current (DX_GPU_CONTEXT) and collects finished queries after each swap (DX_GPU_COLLECT); GPU
// zones on a thread without a context, such as the editor's and the tests', record nothing.
//
//   DX_GPU_ZONE("name")   the GPU time of the commands issued to the end of scope

#include "core/profile.h"

#if DX_PROFILE

#include <glad/gl.h>

#include <tracy/TracyOpenGL.hpp>

#define DX_GPU_CONTEXT()                      \
    do {                                      \
        if (!tracy::GetGpuCtx().ptr) {        \
            TracyGpuContext;                  \
            TracyGpuContextName("OpenGL", 6); \
        }                                     \
    } while (false)
#define DX_GPU_ZONE(name) TracyGpuNamedZone(DX_PROFILE_CONCAT(dxProfileGpuZone, __LINE__), name, tracy::GetGpuCtx().ptr != nullptr)
#define DX_GPU_COLLECT()              \
    do {                              \
        if (tracy::GetGpuCtx().ptr) { \
            TracyGpuCollect;          \
        }                             \
    } while (false)

#else

#define DX_GPU_CONTEXT() static_cast<void>(0)
#define DX_GPU_ZONE(name)
#define DX_GPU_COLLECT() static_cast<void>(0)

#endif

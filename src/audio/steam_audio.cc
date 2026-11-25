#include "audio/steam_audio.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#if DX_STEAM_AUDIO
#include "audio/steam_api.h"
#include "audio/steam_handles.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_loadso.h>
#endif

namespace dx::audio::steam {
#if DX_STEAM_AUDIO
    namespace {
#if defined(_WIN32)
        constexpr const char* LibraryName = "phonon.dll";
#else
        constexpr const char* LibraryName = "libphonon.so";
#endif

        void IPLCALL logMessage(IPLLogLevel level, const char* message)
        {
            if (level == IPL_LOGLEVEL_WARNING) {
                log::warn("DevAudio", "Steam Audio: {}", message ? message : "");
            } else if (level == IPL_LOGLEVEL_ERROR) {
                log::error("DevAudio", "Steam Audio: {}", message ? message : "");
            }
        }

        template <typename Function>
        bool resolve(SDL_SharedObject* object, const char* name, Function& function, bool report)
        {
            function = reinterpret_cast<Function>(SDL_LoadFunction(object, name));
            if (!function && report) {
                log::error("DevAudio", "Steam Audio: {} has no {}", LibraryName, name);
            }
            return function != nullptr;
        }
    }

    const Api* api()
    {
        static const Api* loaded = []() -> const Api* {
            SDL_SharedObject* object = nullptr;
            if (const char* base = SDL_GetBasePath()) {
                object = SDL_LoadObject((std::string(base) + LibraryName).c_str());
            }
            if (!object) {
                object = SDL_LoadObject(LibraryName);
            }
            if (!object) {
                log::error("DevAudio", "Steam Audio: {} could not be loaded", LibraryName);
                return nullptr;
            }
            static Api functions;
            bool found = true;
#define DX_STEAM_RESOLVE(name) found = resolve(object, #name, functions.name, true) && found;
            DX_STEAM_CORE_FUNCTIONS(DX_STEAM_RESOLVE)
#undef DX_STEAM_RESOLVE
            if (!found) {
                SDL_UnloadObject(object);
                return nullptr;
            }
            functions.acoustics = true;
#define DX_STEAM_RESOLVE(name) functions.acoustics = resolve(object, #name, functions.name, true) && functions.acoustics;
            DX_STEAM_ACOUSTIC_FUNCTIONS(DX_STEAM_RESOLVE)
#undef DX_STEAM_RESOLVE
            functions.baking = functions.acoustics;
#define DX_STEAM_RESOLVE(name) functions.baking = resolve(object, #name, functions.name, true) && functions.baking;
            DX_STEAM_BAKE_FUNCTIONS(DX_STEAM_RESOLVE)
#undef DX_STEAM_RESOLVE
            IPLContextSettings settings {};
            settings.version = STEAMAUDIO_VERSION;
            settings.logCallback = logMessage;
            // AVX-512 can throttle the clock for less work done, as Steam Audio's notes warn.
            settings.simdLevel = IPL_SIMDLEVEL_AVX2;
            // A library from another version refuses the context.
            if (functions.iplContextCreate(&settings, &functions.context) != IPL_STATUS_SUCCESS) {
                log::error("DevAudio", "Steam Audio: {} refused a context for version {}.{}.{}", LibraryName, STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH);
                SDL_UnloadObject(object);
                return nullptr;
            }
            return &functions;
        }();
        return loaded;
    }

    bool available()
    {
        return api() != nullptr;
    }

    struct Binaural::Impl {
        const Api* api = nullptr;
        // Before the effects made with it, so that it goes after them.
        HRTF hrtf;
        std::vector<BinauralEffect> effects;
    };

    std::unique_ptr<Binaural> Binaural::create(uint32_t sampleRate, size_t effects)
    {
        const Api* functions = api();
        if (!functions) {
            return nullptr;
        }
        auto impl = std::make_unique<Impl>();
        impl->api = functions;
        IPLAudioSettings audio {};
        audio.samplingRate = static_cast<IPLint32>(sampleRate);
        audio.frameSize = static_cast<IPLint32>(BlockFrames);
        IPLHRTFSettings hrtfSettings {};
        hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
        hrtfSettings.volume = 1.0f;
        // Evens out the built-in HRTF's level between directions: without it a source at the side
        // is about 3 dB louder than one ahead, with it about 1 dB.
        hrtfSettings.normType = IPL_HRTFNORMTYPE_RMS;
        IPLHRTF hrtf = nullptr;
        if (functions->iplHRTFCreate(functions->context, &audio, &hrtfSettings, &hrtf) != IPL_STATUS_SUCCESS) {
            log::error("DevAudio", "Steam Audio: the built-in HRTF could not be created");
            return nullptr;
        }
        impl->hrtf = HRTF(hrtf, { functions });
        for (size_t i = 0; i < effects; i++) {
            IPLBinauralEffectSettings settings {};
            settings.hrtf = hrtf;
            IPLBinauralEffect effect = nullptr;
            if (functions->iplBinauralEffectCreate(functions->context, &audio, &settings, &effect) != IPL_STATUS_SUCCESS) {
                log::error("DevAudio", "Steam Audio: a binaural effect could not be created");
                return nullptr;
            }
            impl->effects.push_back(BinauralEffect(effect, { functions }));
        }
        return std::unique_ptr<Binaural>(new Binaural(std::move(impl)));
    }

    void Binaural::apply(size_t effect, float* mono, float* left, float* right, Direction direction, float blend)
    {
        std::array<float*, 2> ears = { left, right };
        IPLAudioBuffer in { 1, static_cast<IPLint32>(BlockFrames), &mono };
        IPLAudioBuffer out { 2, static_cast<IPLint32>(BlockFrames), ears.data() };
        IPLBinauralEffectParams params {};
        params.direction = { direction.x, direction.y, direction.z };
        // Steam Audio only applies the spatial blend with bilinear interpolation; nearest
        // neighbour ignores it.
        params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
        params.spatialBlend = std::clamp(blend, 0.0f, 1.0f);
        params.hrtf = impl->hrtf.get();
        impl->api->iplBinauralEffectApply(impl->effects[effect].get(), &params, &in, &out);
    }

    bool Binaural::tail(size_t effect, float* left, float* right)
    {
        std::array<float*, 2> ears = { left, right };
        IPLAudioBuffer out { 2, static_cast<IPLint32>(BlockFrames), ears.data() };
        return impl->api->iplBinauralEffectGetTail(impl->effects[effect].get(), &out) == IPL_AUDIOEFFECTSTATE_TAILREMAINING;
    }

    void Binaural::reset(size_t effect)
    {
        impl->api->iplBinauralEffectReset(impl->effects[effect].get());
    }
#else
    bool available()
    {
        return false;
    }

    struct Binaural::Impl {};

    std::unique_ptr<Binaural> Binaural::create(uint32_t, size_t)
    {
        log::warn("DevAudio", "Steam Audio: this build has no Steam Audio (AMBROSIA_STEAM_AUDIO is off)");
        return nullptr;
    }

    void Binaural::apply(size_t, float*, float*, float*, Direction, float)
    {
    }

    bool Binaural::tail(size_t, float*, float*)
    {
        return false;
    }

    void Binaural::reset(size_t)
    {
    }
#endif

    Binaural::Binaural(std::unique_ptr<Impl> implementation)
        : impl(std::move(implementation))
    {
    }

    Binaural::~Binaural() = default;

    float Binaural::loudness(float blend)
    {
        // Measured with white noise at 44.1 kHz through the built-in HRTF with RMS normalisation
        // and bilinear interpolation: the output power over input power, averaged over 36
        // directions around the listener, is 2.0, 1.435, 1.270, 1.272 and 1.372 at blends of 0,
        // 1/4, 1/2, 3/4 and 1. These are its inverse square roots. At a blend of 0 both ears get
        // the input, so the gain is Galaxy's centred pan, 1/sqrt(2).
        static constexpr std::array<float, 5> gains = { 0.70711f, 0.83478f, 0.88736f, 0.88666f, 0.85374f };
        float position = std::clamp(blend, 0.0f, 1.0f) * 4.0f;
        size_t index = std::min<size_t>(static_cast<size_t>(position), 3);
        float fraction = position - static_cast<float>(index);
        return gains[index] + (gains[index + 1] - gains[index]) * fraction;
    }
}

#include "audio/output.h"

#include "core/log.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <vector>

// The mix goes to an SDL audio stream, which SDL's audio thread pulls, converting to the device's
// format.
namespace dx::audio::output {
    namespace {
        struct Device {
            SDL_AudioStream* stream = nullptr;
            std::vector<float> buffer;
            Mix mix = nullptr;
            void* user = nullptr;
        };

        Device& device()
        {
            static Device instance;
            return instance;
        }

        void SDLCALL streamCallback(void* user, SDL_AudioStream* stream, int additional, int)
        {
            Device& d = *static_cast<Device*>(user);
            size_t frames = static_cast<size_t>(std::max(additional, 0)) / (2 * sizeof(float));
            if (frames == 0) {
                return;
            }
            if (d.buffer.size() < frames * 2) {
                d.buffer.resize(frames * 2);
            }
            d.mix(d.user, d.buffer.data(), frames);
            SDL_PutAudioStreamData(stream, d.buffer.data(), static_cast<int>(frames * 2 * sizeof(float)));
        }

        // Starts SDL's audio with its usual drivers, or with the dummy driver, which plays nowhere
        // at the pace of a device, and opens the default device there at its own rate, so SDL does
        // not resample the mix. The stream starts pulling at once.
        bool openWith(bool dummy, uint32_t& rate, Device& d)
        {
            SDL_SetHint(SDL_HINT_AUDIO_DRIVER, dummy ? "dummy" : nullptr);
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                return false;
            }
            SDL_AudioSpec format {};
            if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &format, nullptr) && format.freq > 0) {
                rate = static_cast<uint32_t>(format.freq);
            }
            SDL_AudioSpec spec { SDL_AUDIO_F32, 2, static_cast<int>(rate) };
            d.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, streamCallback, &d);
            if (d.stream && SDL_ResumeAudioStreamDevice(d.stream)) {
                return true;
            }
            SDL_DestroyAudioStream(d.stream);
            d.stream = nullptr;
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
    }

    // Where no device opens, the dummy driver instead, so the game runs as with sound.
    bool open(uint32_t& rate, Mix mix, void* user, bool nullDevice)
    {
        Device& d = device();
        d.mix = mix;
        d.user = user;
        if (!nullDevice) {
            if (openWith(false, rate, d)) {
                return true;
            }
            log::warn("DevAudio", "no sound device ({}); playing nowhere", SDL_GetError());
        }
        if (openWith(true, rate, d)) {
            return true;
        }
        log::error("DevAudio", "{}", SDL_GetError());
        return false;
    }

    // The stream has been pulling since open.
    bool start()
    {
        return true;
    }

    void close()
    {
        Device& d = device();
        SDL_DestroyAudioStream(d.stream);
        d.stream = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

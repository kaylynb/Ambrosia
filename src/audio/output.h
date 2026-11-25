#pragma once

#include <cstddef>
#include <cstdint>

// The sound output device, an SDL audio stream on every platform (audio/output.cc). The mixer stays
// in audio.cc; the device only asks it for samples.
namespace dx::audio::output {
    // Fills out with frames stereo float frames, on the device's own thread.
    using Mix = void (*)(void* user, float* out, size_t frames);

    // Opens the default playback device for stereo float samples at rate, which becomes the rate
    // the device runs at. Where there is no device at all the output still opens and plays
    // nowhere, or it fails and there is no sound. With nullDevice it plays nowhere whatever
    // devices there are, calling mix at the pace a device would. The device may begin calling
    // mix here.
    bool open(uint32_t& rate, Mix mix, void* user, bool nullDevice);
    // Makes sure the device is calling mix. False when it will not start; close it then.
    bool start();
    void close();
}

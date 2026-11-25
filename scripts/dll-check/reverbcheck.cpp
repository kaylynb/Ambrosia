// Runs Galaxy.dll's software reverb and compares it with Ambrosia's Reverb
// (src/audio/audio.cc configure and process, copied here). 32-bit only; build
// as for firecheck.cpp and run as reverbcheck.exe <game>\System.
//
// By default it runs the SSE filter, which
// Galaxy picks on any SSE machine, against Ambrosia on an impulse, noise and a
// sine mixed over an existing signal, for several call sizes: these match
// exactly. Other modes, for the pre-SSE MMX filter
// which works in 16-bit fixed point:
//   MMX=1            compare it with Ambrosia as is
//   MMX=1 GALAXY=1   with the MMX filter's quirks modeled (a stage-0 delay one
//                    frame longer inside each call, one low-pass state carried
//                    through the stages, no left cross-feed into the right on a
//                    call's first frame): matches to about 2 LSB
//   SCAN=1 GALAXY=1  an impulse at every frame 0 to 2799
//   SUPER=<n> GALAXY=1  superposition checks; SUPER=-<n> is noise over n frames
// Run the MMX modes on their own: the 3DNow! filter faults on Intel CPUs and
// leaves the FPU state dirty, so it is left out.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using Ext = long double;
constexpr size_t ReverbFrames = 16384;

struct ReverbSettings {
    float volume = 0;
    float cutoff = 0;
    std::array<float, 6> delay {};
    std::array<float, 6> gain {};
};

struct Reverb {
    bool enabled = false;
    size_t position = 0;
    std::array<size_t, 6> delays {};
    std::array<float, 6> direct {};
    std::array<float, 6> feedback {};
    std::array<float, 6> through {};
    std::array<float, 6> lowpass {};
    std::array<float, 2> last {};
    std::array<float, 2> shared {};
    float smoothing = 0;
    float keep = 0;
    float volume = 0;
    std::vector<float> lines;

    void configure(const ReverbSettings& settings, uint32_t rate)
    {
        enabled = settings.volume != 0;
        Ext bend = Ext(1) - cos(static_cast<Ext>(settings.cutoff) * static_cast<Ext>(6.282f) / static_cast<Ext>(rate));
        smoothing = static_cast<float>(sqrt((bend + Ext(2)) * bend) - bend);
        keep = static_cast<float>(Ext(1) - static_cast<Ext>(smoothing));
        volume = settings.volume;
        for (size_t i = 0; i < 6; i++) {
            float delay = settings.delay[i] == 0.0f ? 0.001f : settings.delay[i];
            Ext amount = Ext(1) - static_cast<Ext>(settings.gain[i]);
            if (amount == Ext(1)) {
                amount = static_cast<Ext>(0.999f);
            }
            delays[i] = static_cast<size_t>(static_cast<Ext>(rate) * static_cast<Ext>(delay)) + (getenv("DELAY_PLUS_ONE") ? 1 : 0);
            direct[i] = static_cast<float>(-amount);
            feedback[i] = static_cast<float>(amount);
            through[i] = static_cast<float>(Ext(1) - amount * amount);
        }
        lines.assign(3 * ReverbFrames * 2, 0.0f);
        lowpass.fill(0.0f);
        last.fill(0.0f);
        position = 0;
    }

    void process(const float* send, float* stereo, size_t frames)
    {
        const size_t mask = ReverbFrames - 1;
        for (size_t n = 0; n < frames; n++) {
            bool galaxy = getenv("GALAXY") != nullptr;
            std::array<float, 2> wave = { (send[n * 2] + last[1]) * 0.5f, (send[n * 2 + 1] + ((galaxy && n == 0) ? 0.0f : last[0])) * 0.5f };
            for (size_t stage = 0; stage < 3; stage++) {
                float* line = lines.data() + stage * ReverbFrames * 2;
                for (size_t channel = 0; channel < 2; channel++) {
                    size_t tap = stage * 2 + channel;
                    size_t extra = (galaxy && stage == 0 && n > 0) ? 1 : 0;
                    size_t read = (position + ReverbFrames - delays[tap] - extra) & mask;
                    float delayed = line[read * 2 + channel];
                    float out = wave[channel] * direct[tap];
                    if (galaxy) {
                        // One low-pass state per channel, carried from stage to stage.
                        shared[channel] = delayed * smoothing * feedback[tap] + shared[channel] * keep;
                        line[position * 2 + channel] = wave[channel] + shared[channel];
                    } else {
                        lowpass[tap] = delayed * smoothing + lowpass[tap] * keep;
                        line[position * 2 + channel] = wave[channel] + lowpass[tap] * feedback[tap];
                    }
                    wave[channel] = out + delayed * through[tap];
                }
            }
            last = wave;
            stereo[n * 2] += wave[0] * volume;
            stereo[n * 2 + 1] += wave[1] * volume;
            position = (position + 1) & mask;
        }
    }
};

typedef int(__cdecl* InitFn)(void* state, void* params);
typedef void(__cdecl* ProcFn)(void* state, int16_t* out, int16_t* in, int frames, int zero);

// Returns 0 on success, -1 on a fault, or the init function's error.
static int runGuarded(InitFn init, ProcFn proc, void* state, void* params, int16_t* out, int16_t* in, int frames)
{
    __try {
        int r = init(state, params);
        if (r != 0) {
            return r;
        }
        for (int start = 0; start < frames; start += 512) {
            int count = frames - start < 512 ? frames - start : 512;
            proc(state, out + start * 2, in + start * 2, count, 0);
        }
        __asm emms;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        __asm emms;
        printf("fault %lx\n", GetExceptionCode());
        return -1;
    }
}

// Copies everything into 16-byte aligned memory first, as the SSE filter needs.
static int runOriginal(InitFn init, ProcFn proc, void* state, void* params, int16_t* out, int16_t* in, int frames)
{
    size_t stateSize = 0x60188 + 64;
    uint8_t* alignedState = static_cast<uint8_t*>(_aligned_malloc(stateSize, 16));
    uint8_t* alignedParams = static_cast<uint8_t*>(_aligned_malloc(0x1b * 4, 16));
    int16_t* alignedOut = static_cast<int16_t*>(_aligned_malloc(frames * 4 + 64, 16));
    int16_t* alignedIn = static_cast<int16_t*>(_aligned_malloc(frames * 4 + 64, 16));
    memset(alignedState, 0, stateSize);
    memcpy(alignedParams, params, 0x1b * 4);
    memcpy(alignedOut, out, frames * 4);
    memcpy(alignedIn, in, frames * 4);
    int r = runGuarded(init, proc, alignedState, alignedParams, alignedOut, alignedIn, frames);
    memcpy(out, alignedOut, frames * 4);
    memcpy(state, alignedState, 0x30138);
    _aligned_free(alignedState);
    _aligned_free(alignedParams);
    _aligned_free(alignedOut);
    _aligned_free(alignedIn);
    return r;
}

typedef void(__cdecl* ProcFloatFn)(void* state, float* out, float* in, int frames, int zero);

// The SSE filter takes float buffers. Returns 0, -1 on a fault, or init's error.
static int runSseGuarded(InitFn init, ProcFloatFn proc, void* state, void* params, float* out, float* in, int frames, int chunk)
{
    __try {
        int r = init(state, params);
        if (r != 0) {
            return r;
        }
        for (int start = 0; start < frames; start += chunk) {
            int count = frames - start < chunk ? frames - start : chunk;
            proc(state, out + start * 2, in + start * 2, count, 0);
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("fault %lx\n", GetExceptionCode());
        return -1;
    }
}

static int compareSse(uint8_t* (*at)(uint32_t), const float* params, const ReverbSettings& settings, uint32_t rate)
{
    const int frames = 44100;
    std::mt19937 rng(11);
    int failures = 0;
    for (int signal = 0; signal < 3; signal++) {
        for (int chunk : { 512, 1000, 44100 }) {
            float* in = static_cast<float*>(_aligned_malloc(frames * 8 + 64, 16));
            float* out = static_cast<float*>(_aligned_malloc(frames * 8 + 64, 16));
            uint8_t* state = static_cast<uint8_t*>(_aligned_malloc(0x60188 + 64, 16));
            float* p = static_cast<float*>(_aligned_malloc(0x1b * 4, 16));
            memset(in, 0, frames * 8);
            memset(out, 0, frames * 8);
            memset(state, 0, 0x60188);
            memcpy(p, params, 0x1b * 4);
            if (signal == 0) {
                in[0] = 0.5f;
                in[1] = -0.375f;
            } else if (signal == 1) {
                for (int i = 0; i < frames / 4; i++) {
                    in[i * 2] = (static_cast<int>(rng() % 20001) - 10000) / 40000.0f;
                    in[i * 2 + 1] = (static_cast<int>(rng() % 20001) - 10000) / 40000.0f;
                }
            } else {
                for (int i = 0; i < frames; i++) {
                    out[i * 2] = 0.25f;
                    out[i * 2 + 1] = -0.125f;
                    in[i * 2] = std::sin(i * 0.01f) * 0.3f;
                    in[i * 2 + 1] = std::cos(i * 0.013f) * 0.3f;
                }
            }
            std::vector<float> mixed(out, out + frames * 2);
            Reverb ours;
            ours.configure(settings, rate);
            for (int start = 0; start < frames; start += chunk) {
                int count = frames - start < chunk ? frames - start : chunk;
                ours.process(in + start * 2, mixed.data() + start * 2, count);
            }
            int r = runSseGuarded(reinterpret_cast<InitFn>(at(0x106109f0)), reinterpret_cast<ProcFloatFn>(at(0x10622d33)), state, p, out, in, frames, chunk);
            if (r != 0) {
                printf("SSE filter did not run (%d)\n", r);
                return 1;
            }
            double maxDiff = 0;
            double maxValue = 0;
            int where = 0;
            for (int i = 0; i < frames * 2; i++) {
                double d = std::fabs(double(out[i]) - mixed[i]);
                maxValue = std::max(maxValue, std::fabs(double(mixed[i])));
                if (d > maxDiff) {
                    maxDiff = d;
                    where = i;
                }
            }
            printf("SSE filter, %s, calls of %d frames: max difference %g (largest value %g) at frame %d ch %d: original %.9g, Ambrosia %.9g\n", signal == 0 ? "impulse" : signal == 1 ? "noise"
                                                                                                                                                                                       : "sine over existing mix",
                chunk, maxDiff, maxValue, where / 2, where % 2, out[where], mixed[where]);
            failures += maxDiff > 1e-5;
            _aligned_free(in);
            _aligned_free(out);
            _aligned_free(state);
            _aligned_free(p);
        }
    }
    return failures;
}

static uint8_t* galaxyBase = nullptr;

static uint8_t* galaxyAt(uint32_t address)
{
    return galaxyBase + (address - 0x10600000);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetDllDirectoryA(argv[1]);
    HMODULE galaxy = LoadLibraryA("Galaxy.dll");
    if (!galaxy) {
        printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    uint8_t* base = reinterpret_cast<uint8_t*>(galaxy);
    auto at = [&](uint32_t address) { return base + (address - 0x10600000); };
    printf("Galaxy.dll at %p\n", base);
    const uint32_t rate = 44100;
    *reinterpret_cast<uint32_t*>(at(0x10697d18)) = rate;
    *at(0x1069817a) = 0;

    ReverbSettings settings;
    settings.volume = 0.5f;
    settings.cutoff = 6000.0f;
    settings.delay = { 0.031f, 0.047f, 0.069f, 0.083f, 0.107f, 0.129f };
    settings.gain = { 0.55f, 0.6f, 0.45f, 0.5f, 0.35f, 0.4f };
    std::array<float, 0x1b> params {};
    params[3] = settings.volume;
    params[4] = settings.cutoff;
    for (int i = 0; i < 6; i++) {
        params[13 + 2 * i] = settings.delay[i];
        params[14 + 2 * i] = settings.gain[i];
    }

    struct {
        const char* name;
        uint32_t init;
        uint32_t proc;
        size_t size;
    } variants[] = {
        { "plain", 0x1060f660, 0x1062135b, 0x30138 },
        { "third", 0x106109f0, 0x10622d33, 0x60188 },
    };

    if (!getenv("MMX") && !getenv("SCAN") && !getenv("SUPER")) {
        galaxyBase = base;
        int f = compareSse(galaxyAt, params.data(), settings, rate);
        printf("%s\n", f ? "SSE MISMATCH" : "SSE ALL MATCH");
        return f;
    }
    if (getenv("SUPER")) {
        const int n = 6000;
        auto runBoth = [&](const std::vector<int16_t>& input, std::vector<int16_t>& out, std::vector<float>& mixed) {
            Reverb ours;
            ours.configure(settings, rate);
            std::vector<float> send(n * 2);
            for (int i = 0; i < n * 2; i++) {
                send[i] = input[i] / 32768.0f;
            }
            mixed.assign(n * 2, 0.0f);
            for (int start = 0; start < n; start += 512) {
                int count = n - start < 512 ? n - start : 512;
                ours.process(send.data() + start * 2, mixed.data() + start * 2, count);
            }
            std::vector<uint8_t> state(0x30138 + 64, 0);
            auto p = params;
            out.assign(n * 2, 0);
            std::vector<int16_t> in = input;
            runOriginal(reinterpret_cast<InitFn>(at(0x1060f660)), reinterpret_cast<ProcFn>(at(0x1062135b)), state.data(), p.data(), out.data(), in.data(), n);
        };
        int a = atoi(getenv("SUPER"));
        if (a < 0) {
            std::mt19937 r(3);
            std::vector<int16_t> noise(n * 2, 0);
            for (int i = 0; i < -a; i++) {
                noise[i * 2] = static_cast<int16_t>(static_cast<int>(r() % 2001) - 1000);
                noise[i * 2 + 1] = static_cast<int16_t>(static_cast<int>(r() % 2001) - 1000);
            }
            std::vector<int16_t> o;
            std::vector<float> m;
            runBoth(noise, o, m);
            double worst = 0;
            int where = 0;
            double sig = 0;
            for (int i = 0; i < n * 2; i++) {
                double c = std::fabs(o[i] - m[i] * 32768.0);
                sig = std::max(sig, std::fabs(m[i] * 32768.0));
                if (c > worst) {
                    worst = c;
                    where = i;
                }
            }
            printf("noise over %d frames: worst difference %.1f at frame %d ch %d (original %d, Ambrosia %.1f), largest output %.0f\n", -a, worst, where / 2, where % 2, o[where], m[where] * 32768.0, sig);
            return 0;
        }
        std::vector<int16_t> ia(n * 2, 0), ib(n * 2, 0), iab(n * 2, 0);
        ia[0] = 8000;
        ib[a * 2 + 1] = 8000;
        iab[0] = 8000;
        iab[a * 2 + 1] = 8000;
        std::vector<int16_t> oa, ob, oab;
        std::vector<float> ma, mb, mab;
        runBoth(ia, oa, ma);
        runBoth(ib, ob, mb);
        runBoth(iab, oab, mab);
        double galaxyNonlinear = 0, oursNonlinear = 0, cross = 0;
        int where = 0;
        for (int i = 0; i < n * 2; i++) {
            galaxyNonlinear = std::max(galaxyNonlinear, std::fabs(double(oab[i]) - oa[i] - ob[i]));
            oursNonlinear = std::max(oursNonlinear, std::fabs(double(mab[i]) - ma[i] - mb[i]) * 32768.0);
            double c = std::fabs(oab[i] - mab[i] * 32768.0);
            if (c > cross) {
                cross = c;
                where = i;
            }
        }
        printf("second impulse at %d: Galaxy superposition error %.1f, Ambrosia %.1f, Galaxy against Ambrosia %.1f at frame %d ch %d\n", a, galaxyNonlinear, oursNonlinear, cross, where / 2, where % 2);
        return 0;
    }
    if (getenv("SCAN")) {
        const int scanFrames = 6000;
        int bad = 0;
        for (int k = 0; k < 2800; k++) {
            std::vector<int16_t> input(scanFrames * 2, 0);
            input[k * 2] = getenv("RIGHT_ONLY") ? 0 : 16000;
            input[k * 2 + 1] = getenv("LEFT_ONLY") ? 0 : -12000;
            Reverb ours;
            ours.configure(settings, rate);
            std::vector<float> send(scanFrames * 2);
            for (int i = 0; i < scanFrames * 2; i++) {
                send[i] = input[i] / 32768.0f;
            }
            std::vector<float> mixed(scanFrames * 2, 0.0f);
            for (int start = 0; start < scanFrames; start += 512) {
                int count = scanFrames - start < 512 ? scanFrames - start : 512;
                ours.process(send.data() + start * 2, mixed.data() + start * 2, count);
            }
            std::vector<uint8_t> state(0x30138 + 64, 0);
            auto p = params;
            std::vector<int16_t> out(scanFrames * 2, 0);
            runOriginal(reinterpret_cast<InitFn>(at(0x1060f660)), reinterpret_cast<ProcFn>(at(0x1062135b)), state.data(), p.data(), out.data(), input.data(), scanFrames);
            double maxDiff = 0;
            int where = 0;
            for (int i = 0; i < scanFrames * 2; i++) {
                double diff = std::fabs(out[i] - mixed[i] * 32768.0);
                if (diff > maxDiff) {
                    maxDiff = diff;
                    where = i;
                }
            }
            if (maxDiff > 8) {
                if (bad < 40) {
                    printf("impulse at %d (offset %d in its call): max difference %.1f at frame %d ch %d (original %d, Ambrosia %.1f)\n", k, k % 512, maxDiff, where / 2, where % 2, out[where], mixed[where] * 32768.0);
                }
                bad++;
            }
        }
        printf("%d of 2800 impulse positions differ\n", bad);
        return 0;
    }
    const int frames = 44100;
    std::mt19937 rng(7);
    for (int signal = 0; signal < 2; signal++) {
        std::vector<int16_t> input(frames * 2, 0);
        int amp = getenv("AMP") ? atoi(getenv("AMP")) : 8000;
        if (signal == 0) {
            int at = getenv("IMPULSE_AT") ? atoi(getenv("IMPULSE_AT")) : 0;
            input[at * 2] = 16000;
            input[at * 2 + 1] = -12000;
        } else {
            for (int i = 0; i < frames / 4; i++) {
                input[i * 2] = static_cast<int16_t>(static_cast<int>(rng() % (2 * amp + 1)) - amp);
                input[i * 2 + 1] = static_cast<int16_t>(static_cast<int>(rng() % (2 * amp + 1)) - amp);
            }
        }
        Reverb ours;
        ours.configure(settings, rate);
        std::vector<float> send(frames * 2);
        for (int i = 0; i < frames * 2; i++) {
            send[i] = input[i] / 32768.0f;
        }
        std::vector<float> mixed(frames * 2, 0.0f);
        for (int start = 0; start < frames; start += 512) {
            int count = frames - start < 512 ? frames - start : 512;
            ours.process(send.data() + start * 2, mixed.data() + start * 2, count);
        }
        for (auto& v : variants) {
            std::vector<uint8_t> state(v.size + 64, 0);
            auto paramsCopy = params;
            std::vector<int16_t> out(frames * 2, 0);
            int r = runOriginal(reinterpret_cast<InitFn>(at(v.init)), reinterpret_cast<ProcFn>(at(v.proc)), state.data(), paramsCopy.data(), out.data(), input.data(), frames);
            if (r != 0) {
                printf("%s: did not run (%d)\n", v.name, r);
                continue;
            }
            if (signal == 0) {
                // Re-run init only, to dump what it writes.
                std::vector<uint8_t> fresh(v.size + 64, 0);
                auto p2 = params;
                reinterpret_cast<InitFn>(at(v.init))(fresh.data(), p2.data());
                int32_t* d = reinterpret_cast<int32_t*>(fresh.data());
                int16_t* w = reinterpret_cast<int16_t*>(fresh.data());
                printf("  dwords 0..7:");
                for (int i = 0; i < 8; i++) {
                    printf(" %d", d[i]);
                }
                printf("\n  words 0x20..0x7e:");
                for (int i = 0x10; i < 0x40; i++) {
                    printf(" %d", w[i]);
                }
                printf("\n  ours: delays");
                for (int i = 0; i < 6; i++) {
                    printf(" %zu", ours.delays[i]);
                }
                printf(" direct");
                for (int i = 0; i < 6; i++) {
                    printf(" %.0f", ours.direct[i] * 32768);
                }
                printf(" feedback");
                for (int i = 0; i < 6; i++) {
                    printf(" %.0f", ours.feedback[i] * 32768);
                }
                printf(" through");
                for (int i = 0; i < 6; i++) {
                    printf(" %.0f", ours.through[i] * 32768);
                }
                printf(" smoothing %.0f keep %.0f volume %.0f\n", ours.smoothing * 32768, ours.keep * 32768, ours.volume * 32768);
            }
            double maxDiff = 0;
            double sumSq = 0;
            double sigSq = 0;
            int firstLarge = -1;
            for (int i = 0; i < frames * 2; i++) {
                double expected = mixed[i] * 32768.0;
                double diff = std::fabs(out[i] - expected);
                if (diff > maxDiff) {
                    maxDiff = diff;
                }
                if (diff > 8 && firstLarge < 0) {
                    firstLarge = i;
                }
                sumSq += diff * diff;
                sigSq += expected * expected;
            }
            printf("%s, %s: max difference %.1f, error %.2f%% of the signal", v.name, signal ? "noise" : "impulse", maxDiff, 100.0 * std::sqrt(sumSq / (sigSq > 0 ? sigSq : 1)));
            if (firstLarge >= 0) {
                printf(", first difference above 64 at frame %d channel %d (original %d, Ambrosia %.1f)", firstLarge / 2, firstLarge % 2, out[firstLarge], mixed[firstLarge] * 32768.0);
            }
            printf("\n");
            if (signal == 0) {
                int shown = 0;
                for (int i = 0; i < frames * 2 && shown < 60; i++) {
                    if (std::abs(out[i]) >= 6 || std::fabs(mixed[i] * 32768.0) >= 6) {
                        printf("    frame %d ch %d: original %d, Ambrosia %.1f\n", i / 2, i % 2, out[i], mixed[i] * 32768.0);
                        shown++;
                    }
                }
            }
        }
    }
    return 0;
}

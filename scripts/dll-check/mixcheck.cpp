// Compares sample interpolation and stepping in Galaxy.dll's SSE mixer with
// Ambrosia's mixVoice (src/audio/audio.cc). On SSE machines, Galaxy copies
// the mixer routine into its mixer buffer.
// 32-bit only; build as for firecheck.cpp and run as mixcheck.exe <game>\System.
// The mixer borrows ESP as a register, so a fault inside it cannot be caught.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

typedef void(__cdecl* MixFn)(float* a, float* b, float* c, int frames, const uint8_t* position, int fraction, int step, int gainL, int gainR, int send1L, int send1R, int send2L, int send2R, int flags);

static uint8_t* base = nullptr;

static uint8_t* at(uint32_t address)
{
    return base + (address - 0x10600000);
}

static int callMix(MixFn mix, float* a, float* b, float* c, int frames, const uint8_t* position, int fraction, int step, int gl, int gr, int s1l, int s1r, int s2l, int s2r, int flags)
{
    __try {
        mix(a, b, c, frames, position, fraction, step, gl, gr, s1l, s1r, s2l, s2r, flags);
        __asm emms;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        __asm emms;
        printf("fault %lx\n", GetExceptionCode());
        return -1;
    }
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
    base = reinterpret_cast<uint8_t*>(galaxy);
    // The mixer uses the table only in other paths; give it one anyway.
    static std::vector<uint8_t> table(0x40000);
    *reinterpret_cast<uint8_t**>(at(0x10698344)) = table.data();
    const float* constant = reinterpret_cast<const float*>(at(0x1064d2f0));
    printf("gain scale constant: %g %g %g %g\n", constant[0], constant[1], constant[2], constant[3]);
    MixFn mix = reinterpret_cast<MixFn>(at(0x10622730));

    std::mt19937 rng(5);
    const int count = 4096;
    std::vector<int16_t> samples16(count + 16);
    std::vector<int8_t> samples8(count + 16);
    for (auto& s : samples16) {
        s = static_cast<int16_t>(static_cast<int>(rng() % 65536) - 32768);
    }
    for (auto& s : samples8) {
        s = static_cast<int8_t>(static_cast<int>(rng() % 256) - 128);
    }
    int failures = 0;
    for (int bits : { 16, 8 }) {
        for (int step : { 0x10000, 0x8000, 0x1a3c2, 0x2f00e, 0x0c351 }) {
            for (int fraction : { 0, 0x4000, 0x9abc }) {
                for (int misalign : { 0, 2 }) {
                    const int frames = 256;
                    float* buffers = static_cast<float*>(_aligned_malloc((frames + 8) * 8 * 3 + 64, 16));
                    memset(buffers, 0, (frames + 8) * 8 * 3 + 64);
                    float* a = buffers + misalign;
                    float* b = buffers + (frames + 8) * 2 + misalign;
                    float* c = buffers + (frames + 8) * 4 + misalign;
                    int gl = 20000, gr = 9000, s1l = 15000, s1r = 7000, s2l = 3000, s2r = 1200;
                    // 16-bit positions count samples: Galaxy reads [position * 2].
                    const uint8_t* start = bits == 16 ? reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(samples16.data() + 10) >> 1) : reinterpret_cast<const uint8_t*>(samples8.data() + 10);
                    if (callMix(mix, a, b, c, frames, start, fraction, step, gl, gr, s1l, s1r, s2l, s2r, bits == 16 ? 1 : 0) != 0) {
                        return 1;
                    }
                    // Ambrosia's mixVoice: weights 32767 - fraction / 2 and fraction / 2
                    // on the sample and the next, the step and fraction made even.
                    double worst = 0;
                    int where = -1;
                    uint32_t frac = static_cast<uint32_t>(fraction) & 0xfffe;
                    uint32_t st = static_cast<uint32_t>(step) & ~1u;
                    size_t index = 10;
                    for (int i = 0; i < frames; i++) {
                        int s0 = bits == 16 ? samples16[index] : samples8[index] * 256;
                        int s1 = bits == 16 ? samples16[index + 1] : samples8[index + 1] * 256;
                        int later = static_cast<int>(frac >> 1);
                        int earlier = 32767 - later;
                        double value = static_cast<double>(s0 * earlier + s1 * later);
                        double expect[6] = { value * gl, value * gr, value * s1l, value * s1r, value * s2l, value * s2r };
                        float got[6] = { a[i * 2], a[i * 2 + 1], b[i * 2], b[i * 2 + 1], c[i * 2], c[i * 2 + 1] };
                        for (int k = 0; k < 6; k++) {
                            double scaled = expect[k] * constant[0];
                            double diff = std::fabs(got[k] - scaled) / std::max(1.0, std::fabs(scaled));
                            if (diff > worst) {
                                worst = diff;
                                where = i * 6 + k;
                            }
                        }
                        uint32_t sum = frac + (st & 0xffff);
                        index += (st >> 16) + (sum >> 16);
                        frac = sum & 0xffff;
                    }
                    bool bad = worst > 1e-6;
                    failures += bad;
                    if (bad || (step == 0x1a3c2 && fraction == 0x4000 && misalign == 0)) {
                        printf("%d-bit, step %x, fraction %x, %saligned: worst relative difference %g (frame %d output %d)\n", bits, step, fraction, misalign ? "mis" : "", worst, where / 6, where % 6);
                    }
                    _aligned_free(buffers);
                }
            }
        }
    }
    printf("%s\n", failures ? "MIXER MISMATCH" : "MIXER ALL MATCH");
    return failures ? 2 : 0;
}

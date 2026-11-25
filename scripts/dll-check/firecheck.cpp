// Runs Fire.dll's fire and water inner loops on random input and compares the
// results byte for byte with the formulas in src/render/procedural.cc
// (calculateFire and calculateWater, copied here). Fire.dll is 32-bit, so this
// builds as a 32-bit program on its own, outside the CMake build:
//
//   vcvarsall.bat x86
//   cl /O2 /EHsc /std:c++17 firecheck.cpp
//   firecheck.exe <game>\System
//
// If either Ambrosia function changes, update its copy below.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

typedef void(__cdecl* FireFn)(uint8_t* bitmap, uint8_t* table, int width, int height);
typedef void(__cdecl* WaterFn)(uint8_t* bitmap, uint8_t* field, uint8_t* tableA, uint8_t* tableB, unsigned width, unsigned height, unsigned parity);

static std::mt19937 rng(12345);

static void ambrosiaFire(std::vector<uint8_t>& pixels, const uint8_t* table, int usize, int vsize, int spread)
{
    std::vector<uint8_t> previous = pixels;
    int umask = usize - 1;
    int vmask = vsize - 1;
    for (int y = 0; y < vsize; y++) {
        uint8_t* line = &pixels[y * usize];
        const uint8_t* row = &previous[((y + spread) & vmask) * usize];
        const uint8_t* lower = &previous[((y + spread + 1) & vmask) * usize];
        for (int x = 0; x < usize; x++) {
            line[x] = table[row[x] + row[(x - 1) & umask] + row[(x + 1) & umask] + lower[x]];
        }
    }
}

static void ambrosiaWater(std::vector<uint8_t>& pixels, std::vector<uint8_t>& fields, const uint8_t* wave, const uint8_t* table, int usize, int vsize, unsigned parity)
{
    int xdim = usize / 2;
    int ydim = vsize / 2;
    int lines = ydim * 2;
    int umask = usize - 1;
    int vmask = vsize - 1;
    auto cell = [&](int line, int x) -> uint8_t& {
        return fields[((line + lines) % lines) * xdim + (x + xdim) % xdim];
    };
    auto pixel = [&](int row, int column) -> uint8_t& {
        return pixels[(row & vmask) * usize + (column & umask)];
    };
    bool even = (parity & 1) == 0;
    for (int y = 0; y < ydim; y++) {
        int line = even ? y * 2 : y * 2 + 1;
        int base = even ? -3 : -2;
        for (int x = 0; x < xdim; x++) {
            int a = cell(line - 1, x + base);
            int c = cell(line - 1, x + base + 1);
            int e = cell(line - 1, x + base + 2);
            int g = cell(line - 1, x + base + 3);
            int b = cell(line + 1, x + base);
            int d = cell(line + 1, x + base + 1);
            int f = cell(line + 1, x + base + 2);
            int h = cell(line + 1, x + base + 3);
            uint8_t& dest = cell(line, x);
            dest = wave[(e + g + f + h) - (dest << 1)];
            int ea = e - a;
            int fb = f - b;
            int gc = g - c;
            int hd = h - d;
            int row = even ? y * 2 - 1 : y * 2;
            int column = even ? x * 2 - 1 : x * 2;
            int right = (gc + hd) >> 1;
            pixel(row, column + 1) = table[right * 2];
            pixel(row + 1, column) = table[fb + hd];
            pixel(row, column) = table[right + ((fb + ea) >> 1)];
            pixel(row + 1, column + 1) = table[hd + hd];
        }
    }
}

static void fill(std::vector<uint8_t>& v)
{
    for (auto& b : v) {
        b = static_cast<uint8_t>(rng());
    }
}

static int countDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int width, const char* what)
{
    int diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] != b[i]) {
            if (diff < 5) {
                printf("    %s differs at row %d col %d: original %d, Ambrosia %d\n", what, static_cast<int>(i) / width, static_cast<int>(i) % width, a[i], b[i]);
            }
            diff++;
        }
    }
    return diff;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetDllDirectoryA(argv[1]);
    printf("loading\n");
    HMODULE fire = LoadLibraryA("Fire.dll");
    if (!fire) {
        printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    uint8_t* base = reinterpret_cast<uint8_t*>(fire);
    printf("Fire.dll at %p\n", base);
    // The inner loops patch their own instructions, so make the image writable.
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    DWORD old = 0;
    if (!VirtualProtect(base, nt->OptionalHeader.SizeOfImage, PAGE_EXECUTE_READWRITE, &old)) {
        printf("VirtualProtect failed: %lu\n", GetLastError());
        return 1;
    }

    // Offsets of the wrappers from the image base.
    struct {
        const char* name;
        uint32_t offset;
        int spread;
    } fires[] = {
        { "fire rising", 0xe460, 1 },
        { "fire still", 0xe494, 0 },
        { "fire rising (PPro)", 0xe4d0, 1 },
        { "fire still (PPro)", 0xe504, 0 },
    };

    int failures = 0;
    for (auto& f : fires) {
        FireFn fn = reinterpret_cast<FireFn>(base + f.offset);
        for (int size : { 8, 16, 64, 256 }) {
            for (int pass = 0; pass < 3; pass++) {
                int w = size;
                int h = pass == 1 ? size / 2 : size;
                std::vector<uint8_t> table(1024);
                fill(table);
                std::vector<uint8_t> original(w * h);
                fill(original);
                std::vector<uint8_t> ours = original;
                fn(original.data(), table.data(), w, h);
                ambrosiaFire(ours, table.data(), w, h, f.spread);
                int d = countDiff(original, ours, w, f.name);
                printf("%s %dx%d: %d differences\n", f.name, w, h, d);
                failures += d != 0;
            }
        }
    }
    WaterFn water = reinterpret_cast<WaterFn>(base + 0xe540);
    for (int size : { 8, 16, 64, 256, -64, -128 }) {
        for (unsigned parity : { 1u, 2u }) {
            int w = size < 0 ? -size : size;
            int h = size < 0 ? -size / 2 : size;
            std::vector<uint8_t> tableA(4096);
            std::vector<uint8_t> tableB(4096);
            fill(tableA);
            fill(tableB);
            std::vector<uint8_t> fieldOriginal((w / 2) * (h / 2) * 2);
            fill(fieldOriginal);
            std::vector<uint8_t> fieldOurs = fieldOriginal;
            std::vector<uint8_t> pixelsOriginal(w * h);
            fill(pixelsOriginal);
            std::vector<uint8_t> pixelsOurs = pixelsOriginal;
            // Galaxy passes the table at +0x8ec as the first and +0xcf0 as the
            // second; both are indexed from 0x200 in.
            water(pixelsOriginal.data(), fieldOriginal.data(), tableA.data() + 1024, tableB.data() + 1024, w, h, parity);
            ambrosiaWater(pixelsOurs, fieldOurs, tableB.data() + 1024 + 512, tableA.data() + 1024 + 512, w, h, parity);
            int dp = countDiff(pixelsOriginal, pixelsOurs, w, "water pixels");
            int df = countDiff(fieldOriginal, fieldOurs, w / 2, "water field");
            printf("water %dx%d parity %u: %d pixel and %d field differences\n", w, h, parity, dp, df);
            failures += (dp != 0) + (df != 0);
        }
    }
    printf("%s\n", failures ? "MISMATCH" : "ALL MATCH");
    return failures ? 2 : 0;
}

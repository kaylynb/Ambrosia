#include "soak_memory.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
// MinGW's C++ library defines NOMINMAX itself.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#else
#include <fstream>
#include <string>
#endif

namespace soak {
    uint64_t privateBytes()
    {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX counters {};
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            return 0;
        }
        return static_cast<uint64_t>(counters.PrivateUsage);
#else
        std::ifstream status("/proc/self/status");
        std::string key;
        while (status >> key) {
            if (key == "RssAnon:") {
                uint64_t kilobytes = 0;
                status >> kilobytes;
                return kilobytes * 1024;
            }
            std::string rest;
            std::getline(status, rest);
        }
        return 0;
#endif
    }

    uint64_t heapAllocatedBytes()
    {
#ifdef _WIN32
        HEAP_SUMMARY summary {};
        summary.cb = sizeof(summary);
        if (!HeapSummary(GetProcessHeap(), 0, &summary)) {
            return 0;
        }
        return static_cast<uint64_t>(summary.cbAllocated);
#else
        return 0;
#endif
    }
}
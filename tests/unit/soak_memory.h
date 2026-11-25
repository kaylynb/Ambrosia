#pragma once

#include <cstdint>

namespace soak {
    // Memory the process has committed for itself: private bytes on Windows
    // (GetProcessMemoryInfo PrivateUsage), anonymous resident memory on Linux.
    // Heap memory that was freed but not returned to the system still counts.
    uint64_t privateBytes();
    // Bytes allocated and not freed on the process heap, which the C runtime's malloc and
    // new use on Windows; free heap kept for reuse does not count. 0 where unsupported.
    uint64_t heapAllocatedBytes();
}

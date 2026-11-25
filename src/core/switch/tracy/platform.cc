#include "core/switch/tracy/platform.h"

#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tracy {
    uint32_t PlatformGetThreadId()
    {
        u64 id = 0;
        svcGetThreadId(&id, threadGetCurHandle());
        return static_cast<uint32_t>(id);
    }

    const char* PlatformGetUserLogin()
    {
        return "switch";
    }

    const char* PlatformGetUserFullName()
    {
        return nullptr;
    }

    // libnx has no host name.
    void PlatformGetHostname(char* name, size_t size)
    {
        std::snprintf(name, size, "nintendo-switch");
    }

    // The viewer only asks for memory with call stacks or code transfer, which are off.
    bool PlatformSafeMemcpy(void* destination, const void* source, size_t size)
    {
        std::memcpy(destination, source, size);
        return true;
    }

    void PlatformAllocatorInit()
    {
    }

    void PlatformAllocatorThreadInit()
    {
    }

    void PlatformAllocatorFinalize()
    {
    }

    void* PlatformMalloc(size_t size)
    {
        return std::malloc(size);
    }

    void PlatformFree(void* memory)
    {
        std::free(memory);
    }

    void* PlatformRealloc(void* memory, size_t size)
    {
        return std::realloc(memory, size);
    }
}

// The client is a static object that starts listening for the viewer as the program starts, before
// main, so the network is brought up before static objects are made: libnx calls userAppInit
// first. Being in the same file as the functions above, it is linked whenever Tracy is.
extern "C" void userAppInit()
{
    socketInitializeDefault();
}

extern "C" void userAppExit()
{
    socketExit();
}

// Every recv in the program comes here (cmake/tracy.cmake links with --wrap=recv), and asks
// for at most 16 KB. The client reads the viewer's requests 128 KB at a time, and Ryujinx (1.3.272
// and 1.3.354) answers a read that large with 0 bytes, the sign of a closed connection, so the
// client hung up about 100 ms after the viewer connected. A short read is allowed anywhere, and
// the requests are a few bytes each.
extern "C" ssize_t __real_recv(int socket, void* data, size_t size, int flags);

extern "C" ssize_t __wrap_recv(int socket, void* data, size_t size, int flags)
{
    constexpr size_t largest = 16384;
    return __real_recv(socket, data, size < largest ? size : largest, flags);
}

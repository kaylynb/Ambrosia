#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>

// What Tracy's client asks of a platform it does not know, through its TRACY_PLATFORM_HEADER
// (cmake/tracy.cmake names this file in Switch profiling builds). Its headers include this one
// wherever they are used, so it declares only.
namespace tracy {
    // The kernel's id of the calling thread (TRACY_HAS_CUSTOM_THREAD_ID).
    uint32_t PlatformGetThreadId();
    // The names a capture is labelled with (TRACY_HAS_CUSTOM_USER_INFO).
    const char* PlatformGetUserLogin();
    const char* PlatformGetUserFullName();
    void PlatformGetHostname(char* name, size_t size);
    // Copies memory the viewer asked to see (TRACY_HAS_CUSTOM_SAFE_COPY). Elsewhere Tracy reads
    // it through a pipe so a bad address fails instead of crashing; libnx has no pipes.
    bool PlatformSafeMemcpy(void* destination, const void* source, size_t size);
    // The client's own memory (TRACY_HAS_CUSTOM_ALLOCATOR), from the C library: its usual
    // allocator, rpmalloc, maps pages with mmap, which libnx lacks.
    void PlatformAllocatorInit();
    void PlatformAllocatorThreadInit();
    void PlatformAllocatorFinalize();
    void* PlatformMalloc(size_t size);
    void PlatformFree(void* memory);
    void* PlatformRealloc(void* memory, size_t size);
}

// libnx's sockets are IPv4 only and its headers have no IPv6 address. The client names one in
// ListenSocket::LocalPort for a socket of family AF_INET6, which with TRACY_ONLY_IPV4 it never
// has; this is the BSD layout, so that code compiles.
struct sockaddr_in6 {
    uint8_t sin6_len;
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    uint8_t sin6_addr[16];
    uint32_t sin6_scope_id;
};

// The global allocation functions, replaced in profiling builds only (src/CMakeLists.txt adds
// this file to each program then) so the trace shows every allocation and release. They
// allocate as the standard ones do.

#include "core/profile.h"

#include <cstdlib>
#include <new>

#if DX_PROFILE

namespace {
    void* allocate(std::size_t size)
    {
        void* memory = std::malloc(size == 0 ? 1 : size);
        if (!memory) {
            throw std::bad_alloc();
        }
        DX_ALLOC(memory, size);
        return memory;
    }

    void* allocateAligned(std::size_t size, std::align_val_t alignment)
    {
        std::size_t align = static_cast<std::size_t>(alignment);
        std::size_t rounded = ((size == 0 ? 1 : size) + align - 1) / align * align;
#ifdef _WIN32
        void* memory = _aligned_malloc(rounded, align);
#else
        void* memory = std::aligned_alloc(align, rounded);
#endif
        if (!memory) {
            throw std::bad_alloc();
        }
        DX_ALLOC(memory, size);
        return memory;
    }

    void release(void* memory) noexcept
    {
        if (memory) {
            DX_FREE(memory);
            std::free(memory);
        }
    }

    void releaseAligned(void* memory) noexcept
    {
        if (memory) {
            DX_FREE(memory);
#ifdef _WIN32
            _aligned_free(memory);
#else
            std::free(memory);
#endif
        }
    }
}

void* operator new(std::size_t size)
{
    return allocate(size);
}

void* operator new[](std::size_t size)
{
    return allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try {
        return allocateAligned(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try {
        return allocateAligned(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* memory) noexcept
{
    release(memory);
}

void operator delete[](void* memory) noexcept
{
    release(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    release(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    release(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept
{
    release(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
    release(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept
{
    releaseAligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept
{
    releaseAligned(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
    releaseAligned(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
    releaseAligned(memory);
}

void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    releaseAligned(memory);
}

void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    releaseAligned(memory);
}

#endif

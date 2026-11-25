# The client of Tracy, the frame profiler, from its source release, for profiling builds
# (AMBROSIA_TRACY). Only its public sources and licence are kept, under TRACY_DOWNLOAD_DIR. The
# version must be the one the viewer and tracy-capture speak, since the two refuse each other's
# protocol otherwise; 0.15.0's tools report commit f2a4f25, the v0.15.0 tag.
#
# Defines the STATIC target Tracy::client, which gives TRACY_ENABLE and the client settings
# below to whatever links it, and DEUSEX_TRACY_LICENSE, the licence to ship with the programs.

set(TRACY_VERSION 0.15.0)
set(TRACY_TARBALL_SHA256 3d57da54aa67d7875da1e8fecd0bd5957675cef73c27dd52bffb6ab8d290a5e6)
set(TRACY_DOWNLOAD_DIR "${CMAKE_SOURCE_DIR}/.cache"
    CACHE PATH "Where the Tracy client source is downloaded and unpacked")
set(TRACY_ARCHIVE ""
    CACHE FILEPATH "A local copy of tracy v${TRACY_VERSION}.tar.gz (GitHub's source archive of the tag) to use instead of downloading it")

set(TRACY_ROOT "${TRACY_DOWNLOAD_DIR}/tracy-${TRACY_VERSION}")
set(TRACY_STAMP "${TRACY_ROOT}/.unpacked-${TRACY_TARBALL_SHA256}")

if(NOT EXISTS "${TRACY_STAMP}")
    file(REMOVE_RECURSE "${TRACY_ROOT}")
    file(MAKE_DIRECTORY "${TRACY_ROOT}")
    if(TRACY_ARCHIVE)
        file(SHA256 "${TRACY_ARCHIVE}" archive_hash)
        if(NOT archive_hash STREQUAL TRACY_TARBALL_SHA256)
            message(FATAL_ERROR "${TRACY_ARCHIVE} has SHA-256 ${archive_hash}, expected ${TRACY_TARBALL_SHA256}")
        endif()
        set(archive "${TRACY_ARCHIVE}")
    else()
        set(archive "${TRACY_ROOT}/tracy-${TRACY_VERSION}.tar.gz")
        set(url "https://github.com/wolfpld/tracy/archive/refs/tags/v${TRACY_VERSION}.tar.gz")
        message(STATUS "Downloading the Tracy ${TRACY_VERSION} source (about 21 MB) from ${url}")
        file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH SHA256=${TRACY_TARBALL_SHA256} STATUS status TLS_VERIFY ON)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${archive}")
            message(FATAL_ERROR "Tracy download failed: ${status}. Pass -DTRACY_ARCHIVE=<tar.gz> to use a local copy, or -DAMBROSIA_TRACY=OFF to build without it.")
        endif()
    endif()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${TRACY_ROOT}"
        PATTERNS
            "tracy-${TRACY_VERSION}/public/*"
            "tracy-${TRACY_VERSION}/LICENSE")
    if(NOT TRACY_ARCHIVE)
        file(REMOVE "${archive}")
    endif()
    file(TOUCH "${TRACY_STAMP}")
endif()

set(TRACY_SOURCE "${TRACY_ROOT}/tracy-${TRACY_VERSION}")
set(DEUSEX_TRACY_LICENSE "${TRACY_SOURCE}/LICENSE")

add_library(tracy_client STATIC "${TRACY_SOURCE}/public/TracyClient.cpp")
add_library(Tracy::client ALIAS tracy_client)
target_include_directories(tracy_client SYSTEM PUBLIC "${TRACY_SOURCE}/public")
target_link_libraries(tracy_client PUBLIC Threads::Threads)

# The client's settings, for every platform here. Only what needs no help from the system is
# on: zones, frames, plots, messages, locks, memory and OpenGL timer queries. Sampling, call
# stacks, system tracing (context switches, vsync) and code transfer are off, as the Switch
# has none of what they need (dlopen, perf, ETW) and the desktop traces stay small.
set(DEUSEX_TRACY_DEFINES
    TRACY_ENABLE
    TRACY_NO_SAMPLING
    TRACY_NO_CALLSTACK
    TRACY_NO_SYSTEM_TRACING
    TRACY_NO_CONTEXT_SWITCH
    TRACY_NO_VSYNC_CAPTURE
    TRACY_NO_CODE_TRANSFER
    TRACY_NO_FRAME_IMAGE)
if(NINTENDO_SWITCH)
    # libnx is none of the systems the client knows, so what it needs from one comes from
    # src/core/switch/tracy (thread ids, names, memory, copying memory), which also brings the
    # network up before the client starts; the client's source is used as it is. It listens on
    # every address, for a viewer on the same network, over IPv4, which is all libnx has, and
    # announces itself there with Tracy's broadcast for the viewer's list. On demand: nothing
    # is kept until a viewer connects, as the console has no room for a whole session's events,
    # and the capture starts at the moment of connecting. Without a crash handler: libnx has no
    # signals, and the game reports crashes itself. Times come from std::chrono, the system
    # tick (52 ns), as the client reads the counter register itself only on Apple and Windows ARM.
    list(APPEND DEUSEX_TRACY_DEFINES
        TRACY_ON_DEMAND
        TRACY_ONLY_IPV4
        TRACY_NO_CRASH_HANDLER
        TRACY_HAS_CUSTOM_THREAD_ID
        TRACY_HAS_CUSTOM_USER_INFO
        TRACY_HAS_CUSTOM_SAFE_COPY
        TRACY_HAS_CUSTOM_ALLOCATOR
        "TRACY_PLATFORM_HEADER=\"${CMAKE_SOURCE_DIR}/src/core/switch/tracy/platform.h\"")
    target_sources(tracy_client PRIVATE "${CMAKE_SOURCE_DIR}/src/core/switch/tracy/platform.cc")
    target_include_directories(tracy_client PRIVATE "${CMAKE_SOURCE_DIR}/src")
    # Reads from the network in smaller pieces, for the emulator (see platform.cc).
    target_link_options(tracy_client INTERFACE -Wl,--wrap=recv)
else()
    # The desktop listens on the loopback address only, and keeps everything from the start
    # until a viewer or tracy-capture connects, so a capture started first sees the map load.
    list(APPEND DEUSEX_TRACY_DEFINES TRACY_ONLY_LOCALHOST)
endif()
target_compile_definitions(tracy_client PUBLIC ${DEUSEX_TRACY_DEFINES})

if(WIN32)
    target_link_libraries(tracy_client PUBLIC ws2_32 dbghelp)
elseif(NOT NINTENDO_SWITCH)
    target_link_libraries(tracy_client PUBLIC ${CMAKE_DL_LIBS})
endif()

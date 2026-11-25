# Steam Audio, Valve's spatial audio library, from its prebuilt release. Only the headers, the
# 64-bit Windows and Linux libraries and the licence files are kept, under
# STEAMAUDIO_DOWNLOAD_DIR. The library is loaded when the program runs (src/audio/steam_audio.cc),
# so the programs start without it and keep the stereo mixer; nothing links to it here.
#
# Defines the INTERFACE target SteamAudio::phonon, giving the headers (src/CMakeLists.txt sets
# DX_STEAM_AUDIO to 0 or 1 for the audio code), and
#   DEUSEX_STEAMAUDIO_RUNTIME   the library to copy next to each program
#   DEUSEX_STEAMAUDIO_LICENSES  the licence files to ship with it

set(STEAMAUDIO_VERSION 4.8.1)
set(STEAMAUDIO_ZIP_SHA256 4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449)
# The release zip carries the third-party notices but not Steam Audio's own Apache 2.0 licence,
# so that comes from the repository at the same tag.
set(STEAMAUDIO_LICENSE_SHA256 cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30)
set(STEAMAUDIO_DOWNLOAD_DIR "${CMAKE_SOURCE_DIR}/.cache"
    CACHE PATH "Where the Steam Audio SDK is downloaded and unpacked")
set(STEAMAUDIO_ARCHIVE ""
    CACHE FILEPATH "A local copy of steamaudio_${STEAMAUDIO_VERSION}.zip to use instead of downloading it")

set(STEAMAUDIO_ROOT "${STEAMAUDIO_DOWNLOAD_DIR}/steamaudio-${STEAMAUDIO_VERSION}")
set(STEAMAUDIO_STAMP "${STEAMAUDIO_ROOT}/.unpacked-${STEAMAUDIO_ZIP_SHA256}")

if(NOT EXISTS "${STEAMAUDIO_STAMP}")
    file(REMOVE_RECURSE "${STEAMAUDIO_ROOT}")
    file(MAKE_DIRECTORY "${STEAMAUDIO_ROOT}")
    if(STEAMAUDIO_ARCHIVE)
        file(SHA256 "${STEAMAUDIO_ARCHIVE}" archive_hash)
        if(NOT archive_hash STREQUAL STEAMAUDIO_ZIP_SHA256)
            message(FATAL_ERROR "${STEAMAUDIO_ARCHIVE} has SHA-256 ${archive_hash}, expected ${STEAMAUDIO_ZIP_SHA256}")
        endif()
        set(archive "${STEAMAUDIO_ARCHIVE}")
    else()
        set(archive "${STEAMAUDIO_ROOT}/steamaudio_${STEAMAUDIO_VERSION}.zip")
        set(url "https://github.com/ValveSoftware/steam-audio/releases/download/v${STEAMAUDIO_VERSION}/steamaudio_${STEAMAUDIO_VERSION}.zip")
        message(STATUS "Downloading Steam Audio ${STEAMAUDIO_VERSION} (about 180 MB) from ${url}")
        file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH SHA256=${STEAMAUDIO_ZIP_SHA256} STATUS status TLS_VERIFY ON)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${archive}")
            message(FATAL_ERROR "Steam Audio download failed: ${status}. Pass -DSTEAMAUDIO_ARCHIVE=<zip> to use a local copy, or -DAMBROSIA_STEAM_AUDIO=OFF to build without it.")
        endif()
    endif()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${STEAMAUDIO_ROOT}"
        PATTERNS
            "steamaudio/include/*"
            "steamaudio/lib/windows-x64/phonon.dll"
            "steamaudio/lib/linux-x64/libphonon.so"
            "steamaudio/THIRDPARTY.md")
    if(NOT STEAMAUDIO_ARCHIVE)
        file(REMOVE "${archive}")
    endif()
    file(DOWNLOAD "https://raw.githubusercontent.com/ValveSoftware/steam-audio/v${STEAMAUDIO_VERSION}/LICENSE.md"
        "${STEAMAUDIO_ROOT}/steamaudio/LICENSE.md" EXPECTED_HASH SHA256=${STEAMAUDIO_LICENSE_SHA256} STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Steam Audio licence download failed: ${status}")
    endif()
    file(TOUCH "${STEAMAUDIO_STAMP}")
endif()

set(STEAMAUDIO_SDK "${STEAMAUDIO_ROOT}/steamaudio")
if(WIN32)
    set(DEUSEX_STEAMAUDIO_RUNTIME "${STEAMAUDIO_SDK}/lib/windows-x64/phonon.dll")
else()
    set(DEUSEX_STEAMAUDIO_RUNTIME "${STEAMAUDIO_SDK}/lib/linux-x64/libphonon.so")
endif()
set(DEUSEX_STEAMAUDIO_LICENSES "${STEAMAUDIO_SDK}/LICENSE.md" "${STEAMAUDIO_SDK}/THIRDPARTY.md")

add_library(SteamAudio::phonon INTERFACE IMPORTED)
set_target_properties(SteamAudio::phonon PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${STEAMAUDIO_SDK}/include")
